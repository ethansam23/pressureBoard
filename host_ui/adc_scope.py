"""Live time-domain scope for the exp/adc-scope firmware branch.

Pairs ONLY with the exp/adc-scope firmware (raw 10-bit ADC stream at
1 Mbaud on P1.0) — the production packet firmware speaks a different
protocol at 9600 (use pressure_monitor.py for that).

Stream frame (2 bytes): b0 = 0x80|ch<<6|seq2<<4|sample[9:6], b1 = sample[5:0].
Burst dump ('T'):  F0 0F, count u16 LE, duration_us u16 LE,
                   count x (lo, hi), 8-bit additive checksum (count..payload).

Keys in the plot window:
  a / b : switch channel (Probe A = AN7/P2.7, Probe B = AN3/P2.3)
  t     : burst capture -> new figure with a microsecond x-axis (+ .npy autosave)
  r     : toggle full-rate recording (.npy)

Usage:
  python adc_scope.py COM5 [--baud 1000000] [--window 5] [--record out.npy]
  python adc_scope.py --selftest

Deps: pip install pyserial matplotlib   (numpy comes with matplotlib)
"""
import argparse
import sys
import threading
import time

import numpy as np

LSB_MV = 5.0                       # native 10-bit LSB ~5 mV (firmware RAW banner)
INVALID = 0x0400                   # firmware marker for a failed conversion


class StreamDecoder:
    """b0/b1 framing with bit7 sync, 2-bit seq drop detection."""

    def __init__(self):
        self.pending_b0 = None
        self.last_seq = None
        self.samples = []          # (sample, ch) since last take()
        self.drops = 0
        self.framing_errors = 0

    def feed(self, data):
        for b in data:
            if self.pending_b0 is None:
                if b & 0x80:
                    self.pending_b0 = b
                else:
                    self.framing_errors += 1
            else:
                if b & 0x80:                   # b0 where b1 expected: resync
                    self.framing_errors += 1
                    self.pending_b0 = b
                    continue
                b0 = self.pending_b0
                self.pending_b0 = None
                sample = ((b0 & 0x0F) << 6) | (b & 0x3F)
                ch = (b0 >> 6) & 1
                seq = (b0 >> 4) & 3
                if self.last_seq is not None:
                    gap = (seq - self.last_seq - 1) & 3
                    self.drops += gap
                self.last_seq = seq
                self.samples.append((sample, ch))

    def take(self):
        out, self.samples = self.samples, []
        return out


class BurstParser:
    """Armed after 'T' is sent; scans for magic, then reads the fixed block."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        """Returns (samples_ndarray, duration_us) when complete, else None."""
        self.buf += data
        while True:
            i = self.buf.find(b"\xF0\x0F")
            if i < 0:
                if len(self.buf) > 4096:
                    del self.buf[:-2]
                return None
            need_hdr = i + 2 + 4
            if len(self.buf) < need_hdr:
                return None
            count = self.buf[i + 2] | (self.buf[i + 3] << 8)
            dur_us = self.buf[i + 4] | (self.buf[i + 5] << 8)
            need = need_hdr + count * 2 + 1
            if count == 0 or count > 8192:
                del self.buf[:i + 2]           # implausible: false magic
                continue
            if len(self.buf) < need:
                return None
            payload = self.buf[need_hdr:need - 1]
            chk = self.buf[need - 1]
            calc = (sum(self.buf[i + 2:need - 1])) & 0xFF
            if calc != chk:
                del self.buf[:i + 2]           # false magic inside stream data
                continue
            samples = np.frombuffer(bytes(payload), dtype="<u2").astype(np.uint16)
            self.buf.clear()
            return samples, dur_us


class Scope:
    def __init__(self, port, baud, window_s, record_path):
        import serial
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, baud, 0.05
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()

        self.dec = StreamDecoder()
        self.burst = None              # BurstParser while armed
        self.lock = threading.Lock()
        self.ring = np.zeros(int(window_s * 55000), dtype=np.uint16)
        self.ring_ch = np.zeros_like(self.ring, dtype=np.uint8)
        self.wr = 0
        self.total = 0
        self.rate_t0 = time.time()
        self.rate_n = 0
        self.rate = 0.0
        self.ch = 0
        self.recording = bool(record_path)
        self.record_path = record_path or "adc_scope_capture.npy"
        self.rec_chunks = []
        self.burst_result = None
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()

    # ---- serial thread ---------------------------------------------------
    def _reader(self):
        while self.alive:
            data = self.ser.read(4096)
            if not data:
                continue
            with self.lock:
                if self.burst is not None:
                    done = self.burst.feed(data)
                    if done is not None:
                        self.burst_result = done
                        self.burst = None
                    continue
                self.dec.feed(data)
                got = self.dec.take()
                if got:
                    arr = np.array(got, dtype=np.uint16)
                    s, c = arr[:, 0], arr[:, 1].astype(np.uint8)
                    n = len(s)
                    idx = (self.wr + np.arange(n)) % len(self.ring)
                    self.ring[idx] = s
                    self.ring_ch[idx] = c
                    self.wr = (self.wr + n) % len(self.ring)
                    self.total += n
                    self.rate_n += n
                    if self.recording:
                        self.rec_chunks.append(arr)
            dt = time.time() - self.rate_t0
            if dt >= 1.0:
                self.rate = self.rate_n / dt
                self.rate_n = 0
                self.rate_t0 = time.time()

    # ---- commands ----------------------------------------------------------
    def send(self, byte):
        self.ser.write(byte)

    def trigger_burst(self):
        with self.lock:
            self.burst = BurstParser()
        self.send(b"T")

    def save_recording(self):
        if self.rec_chunks:
            data = np.concatenate(self.rec_chunks)
            np.save(self.record_path, data)
            print("recorded %d samples -> %s" % (len(data), self.record_path))
            self.rec_chunks = []

    def close(self):
        self.alive = False
        time.sleep(0.1)
        self.ser.close()
        self.save_recording()


def run_gui(scope):
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation

    fig, ax = plt.subplots(figsize=(11, 5))
    fig.canvas.manager.set_window_title("adc_scope — exp/adc-scope branch")
    (ln_min,) = ax.plot([], [], lw=0.7)
    (ln_max,) = ax.plot([], [], lw=0.7, color=ln_min.get_color())
    fill = [None]
    ax.set_xlabel("window (s, right edge = now)")
    ax.set_ylabel("ADC counts (10-bit; 1 LSB ~ %.0f mV)" % LSB_MV)
    ax.set_ylim(0, 1023)
    status = ax.set_title("connecting...")

    cols = 1600

    def frame(_):
        with scope.lock:
            buf = np.roll(scope.ring, -scope.wr)   # oldest..newest
        n = (len(buf) // cols) * cols
        view = buf[len(buf) - n:].reshape(cols, -1)
        lo, hi = view.min(axis=1), view.max(axis=1)
        x = np.linspace(-len(buf) / max(scope.rate, 1.0), 0.0, cols)
        ln_min.set_data(x, lo)
        ln_max.set_data(x, hi)
        if fill[0]:
            fill[0].remove()
        fill[0] = ax.fill_between(x, lo, hi, alpha=0.3,
                                  color=ln_min.get_color())
        ax.set_xlim(x[0], 0.0)
        recent = buf[-int(max(scope.rate, 1000.0)):]
        status.set_text(
            "ch %s   %.1f kS/s   mean %.1f   sigma %.2f   p-p %d   "
            "drops %d   frame-errs %d   %s"
            % ("B" if scope.ch else "A", scope.rate / 1e3,
               recent.mean(), recent.std(), int(recent.max()) - int(recent.min()),
               scope.dec.drops, scope.dec.framing_errors,
               "REC" if scope.recording else ""))
        if scope.burst_result is not None:
            samples, dur_us = scope.burst_result
            scope.burst_result = None
            show_burst(samples, dur_us)
        return ln_min, ln_max

    def show_burst(samples, dur_us):
        name = "burst_%d.npy" % int(time.time())
        np.save(name, samples)
        f2, a2 = plt.subplots(figsize=(11, 4))
        t_us = np.linspace(0.0, float(dur_us), len(samples))
        valid = samples <= 1023
        a2.plot(t_us[valid], samples[valid], lw=0.8)
        if (~valid).any():
            a2.plot(t_us[~valid], np.zeros((~valid).sum()), "rx",
                    label="invalid conversion")
            a2.legend()
        a2.set_xlabel("us  (capture %.2f ms, %.2f us/sample)"
                      % (dur_us / 1000.0, dur_us / max(len(samples), 1)))
        a2.set_ylabel("ADC counts")
        a2.set_title("burst: %d samples (saved %s)" % (len(samples), name))
        f2.show()
        print("burst: %d samples over %d us -> %s" % (len(samples), dur_us, name))

    def on_key(ev):
        if ev.key in ("a", "A"):
            scope.ch = 0
            scope.send(b"A")
        elif ev.key in ("b", "B"):
            scope.ch = 1
            scope.send(b"B")
        elif ev.key in ("t", "T"):
            scope.trigger_burst()
        elif ev.key in ("r", "R"):
            if scope.recording:
                scope.recording = False
                scope.save_recording()
            else:
                scope.recording = True

    fig.canvas.mpl_connect("key_press_event", on_key)
    _anim = FuncAnimation(fig, frame, interval=50, cache_frame_data=False)
    plt.show()
    scope.close()


def selftest():
    def frame_bytes(sample, ch, seq):
        b0 = 0x80 | (ch << 6) | ((seq & 3) << 4) | ((sample >> 6) & 0x0F)
        return bytes([b0, sample & 0x3F])

    d = StreamDecoder()
    stream = b"".join(frame_bytes((37 * i) & 0x3FF, 0, i) for i in range(100))
    d.feed(stream[:17])            # ragged chunk boundaries
    d.feed(stream[17:])
    got = d.take()
    assert len(got) == 100 and d.drops == 0 and d.framing_errors == 0
    assert [s for s, _ in got] == [(37 * i) & 0x3FF for i in range(100)]

    d2 = StreamDecoder()
    d2.feed(stream[:20] + stream[22:])         # drop one whole frame
    assert d2.drops == 1, d2.drops

    bp = BurstParser()
    samples = np.arange(1280, dtype="<u2") % 1024
    payload = samples.tobytes()
    count, dur = 1280, 3200
    hdr = bytes([count & 0xFF, count >> 8, dur & 0xFF, dur >> 8])
    chk = (sum(hdr) + sum(payload)) & 0xFF
    blob = stream[:8] + b"\xF0\x0F" + hdr + payload + bytes([chk])
    out = None
    for i in range(0, len(blob), 100):         # ragged feeds
        out = bp.feed(blob[i:i + 100]) or out
    assert out is not None
    got_s, got_dur = out
    assert got_dur == dur and len(got_s) == count and (got_s == samples).all()
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?", help="COM port, e.g. COM5")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--window", type=float, default=5.0, help="view seconds")
    ap.add_argument("--record", help="record full-rate stream to this .npy")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return
    if not args.port:
        ap.error("give a COM port or --selftest")
    scope = Scope(args.port, args.baud, args.window, args.record)
    try:
        run_gui(scope)
    except KeyboardInterrupt:
        scope.close()
        sys.exit(0)


if __name__ == "__main__":
    main()
