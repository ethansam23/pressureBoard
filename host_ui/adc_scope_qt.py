"""pyqtgraph front-end for the exp/adc-scope raw ADC stream — smooth 60 fps.

Same wire protocol and serial/decoder engine as adc_scope.py (imported from
it); only the display differs: pyqtgraph's native peak (min/max) downsampling
renders the full-rate ring every frame, and mouse wheel/drag zoom works live.

A configurable post-process filter overlay (orange) can be selected in the
control bar: Moving avg / EMA / Median / Median>MA / Butterworth, all
sharing one 'equivalent MA length N' knob (default 16, matching the
firmware's 16x oversample: the orange trace approximates what candidate
firmware filtering would see; the raw trace is what actually happened).

Keys (click the plot first so it has focus):
  A / B : switch channel      T : burst capture (new window, us axis, .npy)
  R     : toggle recording    Z : toggle Y auto-zoom vs full 0-1023
Mouse: wheel = zoom, drag = pan, right-drag = axis zoom (pyqtgraph standard).

Usage: python adc_scope_qt.py COM5 [--baud 1000000] [--window 5] [--record f.npy]
Deps:  pip install pyserial pyqtgraph PySide6
"""
import argparse
import time

import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets
from scipy import ndimage, signal

from adc_scope import Scope, LSB_MV

AVG_PEN = pg.mkPen("#ff8c00", width=1.5)

FILTERS = ["Off", "Moving avg", "EMA", "Median", "Median>MA", "Butterworth"]


def moving_avg(y, n):
    """Causal moving average; returns len(y)-n+1 points aligned to y[n-1:]."""
    c = np.cumsum(np.insert(y.astype(np.float64), 0, 0.0))
    return (c[n:] - c[:-n]) / float(n)


def apply_filter(y, kind, n, fs):
    """Post-process filter bank. All share the 'equivalent MA length N' knob
    (EMA alpha = 2/(N+1); Butterworth fc matched to the MA's -3 dB point).
    Returns (sample_offset, filtered) — offset aligns output to y[offset:]."""
    y = y.astype(np.float64)
    if kind == "Moving avg":
        return n - 1, moving_avg(y, n)
    if kind == "EMA":
        a = 2.0 / (n + 1.0)
        zi = np.array([(1.0 - a) * y[0]])          # settle at y[0], no ramp-in
        out, _ = signal.lfilter([a], [1.0, a - 1.0], y, zi=zi)
        return 0, out
    if kind == "Median":
        size = min(n | 1, 99)                      # odd; capped for frame rate
        return 0, ndimage.median_filter(y, size=size, mode="nearest")
    if kind == "Median>MA":                        # 5-tap glitch killer, then MA
        pre = ndimage.median_filter(y, size=5, mode="nearest")
        return n - 1, moving_avg(pre, n)
    if kind == "Butterworth":                      # 2nd order, fc = MA -3 dB eq.
        fc = min(0.443 * fs / n, 0.45 * fs)
        sos = signal.butter(2, fc, fs=fs, output="sos")
        zi = signal.sosfilt_zi(sos) * y[0]
        out, _ = signal.sosfilt(sos, y, zi=zi)
        return 0, out
    return 0, y


class ScopeWindow(QtWidgets.QWidget):
    def __init__(self, scope):
        super().__init__()
        self.scope = scope
        self.zoom = True
        self.setWindowTitle("adc_scope_qt — exp/adc-scope branch")

        self.plot = pg.PlotWidget()
        self.plot.setLabel("bottom", "window (s, right edge = now)")
        self.plot.setLabel("left",
                           "ADC counts (10-bit; 1 LSB ~ %.0f mV)" % LSB_MV)
        self.curve = self.plot.plot(pen=pg.mkPen(width=1))
        self.curve.setDownsampling(auto=True, method="peak")
        self.avg_curve = self.plot.plot(pen=AVG_PEN)
        self.avg_curve.setDownsampling(auto=True, method="peak")
        self.plot.setClipToView(True)

        self.cmb_filt = QtWidgets.QComboBox()
        self.cmb_filt.addItems(FILTERS)
        self.cmb_filt.setToolTip(
            "Overlay filter (orange). All use the same N: EMA alpha=2/(N+1),\n"
            "Butterworth fc = the length-N moving average's -3 dB point.\n"
            "Median>MA = 5-tap median (glitch removal) then moving average.")
        self.spin_n = QtWidgets.QSpinBox()
        self.spin_n.setRange(2, 4096)
        self.spin_n.setValue(16)                   # = firmware oversample
        self.spin_n.setSuffix(" samples")
        self.spin_n.setToolTip("16 mirrors the production 16x oversample")
        bar = QtWidgets.QHBoxLayout()
        bar.addWidget(QtWidgets.QLabel("Filter (orange):"))
        bar.addWidget(self.cmb_filt)
        bar.addWidget(self.spin_n)
        bar.addStretch(1)

        lay = QtWidgets.QVBoxLayout(self)
        lay.setContentsMargins(4, 4, 4, 4)
        lay.addLayout(bar)
        lay.addWidget(self.plot)

        self.setFocusPolicy(QtCore.Qt.FocusPolicy.StrongFocus)
        self.plot.setFocusProxy(self)              # clicks on plot -> our keys
        self._apply_zoom()

        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(33)                       # ~30 fps data; render async
        self.stat_div = 0

    def _apply_zoom(self):
        if self.zoom:
            self.plot.enableAutoRange(axis="y")
            self.plot.getViewBox().setAutoVisible(y=True)
        else:
            self.plot.disableAutoRange(axis="y")
            self.plot.setYRange(0, 1023)

    def refresh(self):
        s = self.scope
        with s.lock:
            buf = np.roll(s.ring, -s.wr)
        rate = max(s.rate, 1.0)
        x = np.linspace(-len(buf) / rate, 0.0, len(buf))
        self.curve.setData(x, buf)

        n = self.spin_n.value()
        kind = self.cmb_filt.currentText()
        heavy = kind in ("Median", "Median>MA")    # C, but O(size) per point
        if kind != "Off" and n >= 2 and n < len(buf) and \
                (not heavy or self.stat_div % 3 == 0 or
                 self.avg_curve.xData is None):
            off, out = apply_filter(buf, kind, n, rate)
            self.avg_curve.setData(x[off:], out)
        elif kind == "Off":
            self.avg_curve.setData([], [])

        self.stat_div = (self.stat_div + 1) % 6
        if self.stat_div == 0:                     # title 5x/s is plenty
            recent = buf[-int(max(rate, 1000.0)):]
            self.plot.setTitle(
                "ch %s   %.1f kS/s   mean %.1f   sigma %.2f   p-p %d   "
                "drops %d   frame-errs %d   %s"
                % ("B" if s.ch else "A", s.rate / 1e3, recent.mean(),
                   recent.std(), int(recent.max()) - int(recent.min()),
                   s.dec.drops, s.dec.framing_errors,
                   "REC" if s.recording else ""))

        if s.burst_result is not None:
            samples, dur_us = s.burst_result
            s.burst_result = None
            self.show_burst(samples, dur_us)

    def show_burst(self, samples, dur_us):
        name = "burst_%d.npy" % int(time.time())
        np.save(name, samples)
        t_us = np.linspace(0.0, float(dur_us), len(samples))
        w = pg.plot(t_us, samples,
                    pen=pg.mkPen(width=1),
                    title="burst: %d samples over %.2f ms (%.2f us/sample) — %s"
                    % (len(samples), dur_us / 1000.0,
                       dur_us / max(len(samples), 1), name))
        w.setLabel("bottom", "us")
        w.setLabel("left", "ADC counts")
        n = self.spin_n.value()
        kind = self.cmb_filt.currentText()
        if kind != "Off" and n >= 2 and n < len(samples):
            fs_burst = len(samples) / max(dur_us, 1) * 1e6
            off, out = apply_filter(samples, kind, n, fs_burst)
            w.plot(t_us[off:], out, pen=AVG_PEN)
        if not hasattr(self, "_bursts"):
            self._bursts = []
        self._bursts.append(w)                     # keep a ref or Qt closes it
        print("burst: %d samples over %d us -> %s" % (len(samples), dur_us, name))

    def keyPressEvent(self, ev):
        k = ev.text().lower()
        if k == "a":
            self.scope.ch = 0
            self.scope.send(b"A")
        elif k == "b":
            self.scope.ch = 1
            self.scope.send(b"B")
        elif k == "t":
            self.scope.trigger_burst()
        elif k == "r":
            if self.scope.recording:
                self.scope.recording = False
                self.scope.save_recording()
            else:
                self.scope.recording = True
        elif k == "z":
            self.zoom = not self.zoom
            self._apply_zoom()
        else:
            super().keyPressEvent(ev)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="COM port, e.g. COM5")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--window", type=float, default=5.0, help="view seconds")
    ap.add_argument("--record", help="record full-rate stream to this .npy")
    args = ap.parse_args()

    scope = Scope(args.port, args.baud, args.window, args.record)
    app = pg.mkQApp("adc_scope_qt")
    win = ScopeWindow(scope)
    win.resize(1200, 580)
    win.show()
    try:
        app.exec()
    finally:
        scope.close()


if __name__ == "__main__":
    main()
