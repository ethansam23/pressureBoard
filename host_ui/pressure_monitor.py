#!/usr/bin/env python3
"""
DeepProbe -- Downhole Pressure Probe Monitor (digital-link edition)

Host-side GUI for the TLE9854 pressure-transmitter firmware. The tool taps
the transmitter's single UART line (9600 8N1) — the same wire the downhole
logger reads — and has two modes:

PASSIVE MONITOR (default, and the resting state of the whole system):
    Receive-only. The tool NEVER writes a byte to the serial port — the
    write guard sits at the serial chokepoint, not just in the UI — so it is
    safe to tap the line anywhere: production board, debug board, even while
    a logger is listening. It decodes the binary packet stream (sync/
    checksum validation, deci-bar + fault codes) and shows live value,
    packet-rate/error statistics, and staleness.

BENCH MODE (explicit opt-in; debug firmware only):
    Sends CONSOLE UNLOCK, which SUSPENDS the packet stream and opens the
    firmware's command console on the same line (mutual exclusion — the
    wire carries either packets or text, never both). Commands, calibration
    workflow, and LINKTEST work here. Leaving bench mode sends CONSOLE LOCK
    and the stream resumes. The firmware also re-locks itself after 5 min
    of inactivity or a power cycle.

Run:  python pressure_monitor.py
Deps: pip install customtkinter pyserial
"""

import re
import time
import queue
import threading
from collections import deque

import customtkinter as ctk
import tkinter as tk
import tkinter.messagebox  # noqa: F401  (accessed as tk.messagebox)

import serial
import serial.tools.list_ports

from link_decoder import LinkDecoder, decode_code

# ---- Palette ---------------------------------------------------------------
BG      = "#0f1115"
PANEL   = "#181b22"
PANEL2  = "#262b36"
TEXT    = "#eef1f6"
MUTED   = "#8a93a6"
ACCENT  = "#1fb6d6"   # teal
GREEN   = "#39d98a"
ORANGE  = "#ffb454"
RED     = "#ff5c6c"
BAUD    = 9600        # the logger's rate — console shares it
BAR_TO_PSI = 14.5037738   # display-only unit conversion (firmware is in bar)
FULL_SCALE_BAR = 1000.0   # fixed absolute wire scale (0.1 bar/LSB)
STALE_S = 0.5             # no valid packet for this long -> STALE

# ---- Branding (change these two lines to rename the app) -------------------
APP_NAME    = "DeepProbe"
APP_TAGLINE = "Downhole Pressure Probe Monitor"

# ---- Console-text parsing (bench mode) --------------------------------------
# AUTO line, e.g.:  A:2048 2500mV  B:2052 2505mV  Avg:2050  P:123.400bar  Link:0x04D2 CAL
RE_A     = re.compile(r"A:\s*(\d+)\s+(\d+)\s*mV")
RE_B     = re.compile(r"B:\s*(\d+)\s+(\d+)\s*mV")
RE_AVG   = re.compile(r"Avg:\s*(\d+)")
RE_P     = re.compile(r"P:\s*(uncal|[\d.]+)")
RE_LINKC = re.compile(r"Link:\s*0x([0-9A-Fa-f]{4})")
RE_TAG   = re.compile(r"\b(TST|FLT|CAL|UNC)\b")
# STATUS lines
RE_SLINK = re.compile(r"Link:\s*0x([0-9A-Fa-f]{4})\s+(LIVE|TEST\(!\))\s+mode=(\S+)")
RE_PKTS  = re.compile(r"pkts=(\d+)\s+aborts=(\d+)\s+skips=(\d+)")
RE_FLTS  = re.compile(r"Faults:\s*(.+)")
RE_RATE  = re.compile(r"Rate:\s*(\d+)\s*ms")
RE_THR   = re.compile(r"Thresh:\s*(\d+)")
RE_CAL   = re.compile(r"Cal:\s*(NONE|VALID)")
RE_CALSO = re.compile(r"slope=([-\d.]+)\s+offset=([-\d.]+)")
RE_PROBE = re.compile(r"Probe:\s*(AVG|A|B)")


def is_telemetry(line):
    return ("Link:" in line) and bool(RE_TAG.search(line))


def parse_line(line, st):
    """Update state dict `st` in-place from one console text line."""
    m = RE_A.search(line)
    if m: st["a"], st["a_mv"] = int(m.group(1)), int(m.group(2))
    m = RE_B.search(line)
    if m: st["b"], st["b_mv"] = int(m.group(1)), int(m.group(2))
    m = RE_AVG.search(line)
    if m: st["avg"] = int(m.group(1))
    m = RE_P.search(line)
    if m: st["p_bar"] = None if m.group(1) == "uncal" else float(m.group(1))
    m = RE_TAG.search(line)
    if m:
        st["mode"] = m.group(1)
        st["fault"] = (m.group(1) == "FLT")
    m = RE_SLINK.search(line)
    if m:
        st["link_code"] = int(m.group(1), 16)
        st["link_test"] = m.group(2).startswith("TEST")
        st["link_mode"] = m.group(3)
    m = RE_PKTS.search(line)
    if m:
        st["pkts"], st["aborts"], st["skips"] = (int(m.group(1)),
                                                 int(m.group(2)),
                                                 int(m.group(3)))
    m = RE_FLTS.search(line)
    if m:
        st["faults"] = m.group(1).strip()
        st["fault"] = (st["faults"] != "none")
    m = RE_RATE.search(line)
    if m: st["rate_ms"] = int(m.group(1))
    m = RE_THR.search(line)
    if m: st["thresh"] = int(m.group(1))
    m = RE_CAL.search(line)
    if m: st["cal_valid"] = (m.group(1) == "VALID")
    m = RE_CALSO.search(line)
    if m: st["cal_slope"], st["cal_offset"] = float(m.group(1)), float(m.group(2))
    m = RE_PROBE.search(line)
    if m: st["probe"] = m.group(1)


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


# ---- Serial worker ---------------------------------------------------------
class SerialWorker(threading.Thread):
    """Reads raw chunks (the decoder does the framing). PASSIVE by default:
    send() refuses to touch the wire unless allow_tx has been explicitly
    enabled — this is the structural write guard, independent of the UI."""

    def __init__(self, port, rx_queue):
        super().__init__(daemon=True)
        self.port = port
        self.rx = rx_queue
        self.ser = None
        self._stop = threading.Event()
        self._wlock = threading.Lock()
        self.allow_tx = False          # PASSIVE until bench mode opts in
        self.error = None

    def run(self):
        try:
            self.ser = serial.Serial(self.port, BAUD, timeout=0.1)
        except Exception as e:
            self.error = str(e)
            self.rx.put(("__err__", str(e)))
            return
        self.rx.put(("__open__", self.port))
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(512)
                if chunk:
                    self.rx.put(("raw", (chunk, time.time())))
            except Exception as e:
                self.rx.put(("__err__", str(e)))
                break
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass

    def send(self, cmd):
        if not self.ser:
            return False
        if not self.allow_tx:
            # Structural guard: passive monitor NEVER writes.
            self.rx.put(("__blocked__", cmd))
            return False
        try:
            with self._wlock:
                self.ser.write((cmd + "\r\n").encode("ascii"))
            return True
        except Exception as e:
            self.rx.put(("__err__", str(e)))
            return False

    def stop(self):
        self._stop.set()


# ---- App -------------------------------------------------------------------
class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        ctk.set_appearance_mode("dark")
        self.title(f"{APP_NAME} — {APP_TAGLINE}")
        self.geometry("1120x800")
        self.minsize(940, 660)
        self.configure(fg_color=BG)

        self.worker = None
        self.rx = queue.Queue()
        self.decoder = LinkDecoder()
        self.tele = {}
        self.bench = False              # bench (console) mode active?
        self.history = deque(maxlen=300)
        self.headline = "--"
        self.unit = ""
        self.frac = 0.0
        self.gcolor = ACCENT
        self.last_frame_wall = None     # staleness reference
        self.last_code = None
        self._last_status = 0.0
        self._text_buf = b""
        self.unit_pref = "bar"

        self._fonts()
        self._build()
        self._refresh_ports()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(50, self._poll)

    def _fonts(self):
        self.f_huge  = ctk.CTkFont("Segoe UI", 30, "bold")
        self.f_big   = ctk.CTkFont("Segoe UI", 18, "bold")
        self.f_lbl   = ctk.CTkFont("Segoe UI", 13)
        self.f_val   = ctk.CTkFont("Segoe UI", 22, "bold")
        self.f_small = ctk.CTkFont("Consolas", 12)

    # ---- UI construction --------------------------------------------------
    def _build(self):
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(1, weight=3)
        self.grid_rowconfigure(6, weight=1)

        # Top: connection bar
        top = ctk.CTkFrame(self, fg_color=PANEL, corner_radius=12)
        top.grid(row=0, column=0, sticky="ew", padx=12, pady=(12, 6))
        brand = ctk.CTkFrame(top, fg_color="transparent")
        brand.pack(side="left", padx=(12, 16), pady=8)
        ctk.CTkLabel(brand, text=APP_NAME, font=self.f_big,
                     text_color=ACCENT).pack(side="left")
        ctk.CTkLabel(brand, text="  " + APP_TAGLINE, font=self.f_lbl,
                     text_color=MUTED).pack(side="left", pady=(5, 0))
        self.port_menu = ctk.CTkOptionMenu(top, values=["(no ports)"], width=180,
                                           font=self.f_lbl, fg_color=PANEL2,
                                           button_color=PANEL2)
        self.port_menu.pack(side="left", padx=4)
        ctk.CTkButton(top, text="↻", width=36, command=self._refresh_ports,
                      fg_color=PANEL2).pack(side="left", padx=4)
        self.btn_conn = ctk.CTkButton(top, text="Connect", width=110,
                                      fg_color=ACCENT, command=self._toggle_conn)
        self.btn_conn.pack(side="left", padx=8)
        self.btn_bench = ctk.CTkButton(top, text="Enter Bench Mode", width=150,
                                       fg_color=PANEL2, hover_color=ORANGE,
                                       command=self._toggle_bench)
        self.btn_bench.pack(side="left", padx=8)
        self.lbl_conn = ctk.CTkLabel(top, text="●  disconnected",
                                     font=self.f_lbl, text_color=MUTED)
        self.lbl_conn.pack(side="right", padx=14)
        self.unit_toggle = ctk.CTkSegmentedButton(
            top, values=["bar", "psi"], command=self._set_unit,
            font=self.f_lbl, selected_color=ACCENT)
        self.unit_toggle.set("bar")
        self.unit_toggle.pack(side="right", padx=10, pady=8)

        # Main: gauge card | chart card
        main = ctk.CTkFrame(self, fg_color="transparent")
        main.grid(row=1, column=0, sticky="nsew", padx=12, pady=6)
        main.grid_columnconfigure(0, weight=1, uniform="m")
        main.grid_columnconfigure(1, weight=2, uniform="m")
        main.grid_rowconfigure(0, weight=1)

        gcard = ctk.CTkFrame(main, fg_color=PANEL, corner_radius=12)
        gcard.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        self.lbl_live = ctk.CTkLabel(gcard, text="LIVE (passive monitor)",
                                     font=self.f_lbl, text_color=MUTED)
        self.lbl_live.pack(anchor="w", padx=16, pady=(12, 0))
        self.gauge = tk.Canvas(gcard, bg=PANEL, highlightthickness=0)
        self.gauge.pack(fill="both", expand=True, padx=10, pady=10)

        ccard = ctk.CTkFrame(main, fg_color=PANEL, corner_radius=12)
        ccard.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        ctk.CTkLabel(ccard, text="TREND", font=self.f_lbl, text_color=MUTED).pack(
            anchor="w", padx=16, pady=(12, 0))
        self.chart = tk.Canvas(ccard, bg=PANEL, highlightthickness=0)
        self.chart.pack(fill="both", expand=True, padx=10, pady=10)

        # Stat tiles
        tiles = ctk.CTkFrame(self, fg_color="transparent")
        tiles.grid(row=2, column=0, sticky="ew", padx=12, pady=6)
        self.tile = {}
        names = [("Code", "code"), ("Packets", "pkts"), ("Chk errors", "errs"),
                 ("Rate", "rate"), ("Link", "stale"), ("Fault", "fault")]
        for i, (label, key) in enumerate(names):
            tiles.grid_columnconfigure(i, weight=1)
            f = ctk.CTkFrame(tiles, fg_color=PANEL, corner_radius=10)
            f.grid(row=0, column=i, sticky="ew", padx=4)
            ctk.CTkLabel(f, text=label, font=self.f_lbl, text_color=MUTED).pack(
                pady=(8, 0))
            v = ctk.CTkLabel(f, text="--", font=self.f_val, text_color=TEXT)
            v.pack(pady=(0, 8))
            self.tile[key] = v

        # Config strip (populated from STATUS in bench mode)
        self.cfg = ctk.CTkLabel(self, text="Rate: --   Probe: --   Cal: --   Faults: --",
                                font=self.f_lbl, text_color=MUTED)
        self.cfg.grid(row=3, column=0, sticky="w", padx=20, pady=(0, 4))

        # Quick buttons (bench mode only — disabled while passive)
        qb = ctk.CTkFrame(self, fg_color="transparent")
        qb.grid(row=4, column=0, sticky="ew", padx=12, pady=4)
        quick = [("Stream On/Off", "AUTO"), ("Refresh", "STATUS"),
                 ("Probe A", "PROBE A"), ("Probe AVG", "PROBE AVG"),
                 ("LinkTest OFF", "LINKTEST OFF"),
                 ("Fast 10Hz", "RATE 100"), ("Normal 1Hz", "RATE 1000")]
        self.quick_btns = []
        for label, cmd in quick:
            b = ctk.CTkButton(qb, text=label, command=lambda c=cmd: self._send(c),
                              fg_color=PANEL2, hover_color=ACCENT, width=110,
                              font=self.f_lbl, state="disabled")
            b.pack(side="left", padx=4)
            self.quick_btns.append(b)

        # Passive-mode notice
        self.lbl_mode = ctk.CTkLabel(
            self, font=self.f_lbl, text_color=MUTED,
            text="PASSIVE MONITOR — this tool transmits nothing. "
                 "Enter Bench Mode to send commands (suspends the packet stream).")
        self.lbl_mode.grid(row=5, column=0, sticky="w", padx=20, pady=(0, 2))

        # Raw command + log
        bottom = ctk.CTkFrame(self, fg_color=PANEL, corner_radius=12)
        bottom.grid(row=6, column=0, sticky="nsew", padx=12, pady=(6, 12))
        bottom.grid_columnconfigure(0, weight=1)
        bottom.grid_rowconfigure(1, weight=1)
        row = ctk.CTkFrame(bottom, fg_color="transparent")
        row.grid(row=0, column=0, sticky="ew", padx=10, pady=8)
        row.grid_columnconfigure(0, weight=1)
        self.entry = ctk.CTkEntry(row, placeholder_text="bench-mode command (e.g. CAL ARM, LINKTEST 10000)...",
                                  font=self.f_small)
        self.entry.grid(row=0, column=0, sticky="ew", padx=(0, 8))
        self.entry.bind("<Return>", lambda _e: self._send_entry())
        self.btn_send = ctk.CTkButton(row, text="Send", width=80, fg_color=PANEL2,
                                      state="disabled", command=self._send_entry)
        self.btn_send.grid(row=0, column=1)
        self.log = ctk.CTkTextbox(bottom, font=self.f_small, fg_color=BG,
                                  text_color=MUTED, height=120)
        self.log.grid(row=1, column=0, sticky="nsew", padx=10, pady=(0, 10))

    # ---- Connection -------------------------------------------------------
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_menu.configure(values=ports or ["(no ports)"])
        if ports:
            self.port_menu.set(ports[0])

    def _toggle_conn(self):
        if self.worker:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.port_menu.get()
        if not port or port.startswith("("):
            self._logline("! pick a COM port first")
            return
        self.decoder = LinkDecoder()
        self.last_frame_wall = None
        self.worker = SerialWorker(port, self.rx)
        self.worker.start()
        self.btn_conn.configure(text="Disconnect", fg_color=RED)
        # PASSIVE: deliberately no STATUS/AUTO/UNLOCK on connect.

    def _disconnect(self):
        if self.worker:
            if self.bench:
                self.worker.send("CONSOLE LOCK")   # resume the stream
                time.sleep(0.1)
            self.worker.stop()
            self.worker = None
        self._set_bench_ui(False)
        self.btn_conn.configure(text="Connect", fg_color=ACCENT)
        self.lbl_conn.configure(text="●  disconnected", text_color=MUTED)

    # ---- Bench mode -------------------------------------------------------
    def _toggle_bench(self):
        if not self.worker:
            self._logline("! connect first")
            return
        if self.bench:
            self.worker.send("CONSOLE LOCK")
            self.worker.allow_tx = False
            self._set_bench_ui(False)
            self._logline("-- bench mode OFF: console locked, stream resuming")
        else:
            if not tk.messagebox.askokcancel(
                    "Enter Bench Mode",
                    "This sends CONSOLE UNLOCK, which SUSPENDS the downhole "
                    "packet stream while the console is open (a connected "
                    "logger will see the line go quiet).\n\nDebug firmware "
                    "only — production builds will not respond.\n\nProceed?"):
                return
            self.worker.allow_tx = True
            self.worker.send("CONSOLE UNLOCK")
            self._set_bench_ui(True)
            self._logline("-- bench mode ON: stream suspended, console open")
            self.after(400, lambda: self._send("STATUS"))

    def _set_bench_ui(self, on):
        self.bench = on
        state = "normal" if on else "disabled"
        for b in self.quick_btns:
            b.configure(state=state)
        self.btn_send.configure(state=state,
                                fg_color=ACCENT if on else PANEL2)
        self.btn_bench.configure(
            text="Exit Bench Mode" if on else "Enter Bench Mode",
            fg_color=ORANGE if on else PANEL2)
        self.lbl_live.configure(
            text="LIVE (bench console — stream suspended)" if on
            else "LIVE (passive monitor)")
        self.lbl_mode.configure(
            text="BENCH MODE — packet stream suspended; console owns the line. "
                 "Exit bench mode (or 5-min firmware timeout) resumes it."
            if on else
            "PASSIVE MONITOR — this tool transmits nothing. "
            "Enter Bench Mode to send commands (suspends the packet stream).")

    def _send(self, cmd):
        if self.worker and self.worker.send(cmd):
            self._logline(">> " + cmd)
        elif not self.worker:
            self._logline("! not connected")

    def _send_entry(self):
        cmd = self.entry.get().strip()
        if cmd:
            self._send(cmd)
            self.entry.delete(0, "end")

    # ---- Main poll loop ---------------------------------------------------
    def _poll(self):
        new_data = False
        try:
            for _ in range(200):
                kind, payload = self.rx.get_nowait()
                if kind == "__open__":
                    self.lbl_conn.configure(text="●  connected " + payload,
                                            text_color=GREEN)
                    self._logline("-- connected %s (passive; decoding stream)"
                                  % payload)
                elif kind == "__err__":
                    self.lbl_conn.configure(text="●  error", text_color=RED)
                    self._logline("! " + payload)
                    self._disconnect()
                elif kind == "__blocked__":
                    self._logline("! passive monitor — '%s' NOT sent "
                                  "(enter Bench Mode first)" % payload)
                elif kind == "raw":
                    chunk, t = payload
                    for ev, data in self.decoder.feed(chunk, t=t):
                        if ev == "frame":
                            self._on_frame(data)
                            new_data = True
                        else:
                            self._on_text(data)
        except queue.Empty:
            pass

        # bench mode: periodic STATUS keeps config strip fresh
        if self.worker and self.bench and (time.time() - self._last_status) > 4.0:
            self._last_status = time.time()
            self.worker.send("STATUS")

        if new_data:
            self._update_headline()
            self.history.append(self.frac)
        self._update_tiles()
        self._draw_gauge()
        self._draw_chart()
        self.after(50, self._poll)

    # ---- Stream events ------------------------------------------------------
    def _on_frame(self, code):
        self.last_code = code
        self.last_frame_wall = time.time()
        kind, val = decode_code(code)
        if kind == "pressure":
            self.tele["p_bar"] = val
            self.tele["fault"] = False
            self.tele["status_name"] = None
        elif kind == "status":
            self.tele["status_name"] = val
            self.tele["fault"] = val not in ("NO_READING",)
        else:
            self.tele["status_name"] = f"INVALID 0x{code:04X}"
            self.tele["fault"] = True

    def _on_text(self, data):
        self._text_buf += data
        while b"\n" in self._text_buf:
            line, self._text_buf = self._text_buf.split(b"\n", 1)
            s = line.decode("ascii", "replace").strip("\r\n ")
            if s:
                self._logline(s)
                parse_line(s, self.tele)
                if is_telemetry(s):
                    # bench-mode AUTO line drives the gauge while the
                    # binary stream is suspended
                    self._update_headline()
                    self.history.append(self.frac)

    # ---- Display ------------------------------------------------------------
    def _set_unit(self, val):
        self.unit_pref = val
        self._update_headline()
        self._update_tiles()
        self._draw_gauge()

    def _update_headline(self):
        st = self.tele
        name = st.get("status_name")
        p = st.get("p_bar")
        if name:
            self.headline = name
            self.unit = "status code"
            self.frac = 0.0
            self.gcolor = RED if st.get("fault") else ORANGE
            return
        if p is not None:
            if self.unit_pref == "psi":
                self.headline = f"{p * BAR_TO_PSI:.0f}"
                self.unit = "psi"
            else:
                self.headline = f"{p:.1f}"
                self.unit = "bar"
            self.frac = clamp(p / FULL_SCALE_BAR, 0, 1)
            mode = st.get("mode")
            self.gcolor = {"FLT": RED, "TST": ORANGE}.get(mode, GREEN)
        else:
            self.headline = "--"
            self.unit = "waiting for stream"
            self.frac = 0.0
            self.gcolor = ACCENT

    def _update_tiles(self):
        d = self.decoder
        st = self.tele
        if self.last_code is not None:
            self.tile["code"].configure(text=f"0x{self.last_code:04X}")
        self.tile["pkts"].configure(text=str(d.frames_ok))
        self.tile["errs"].configure(
            text=str(d.checksum_errors),
            text_color=RED if d.checksum_errors else GREEN)
        r = d.frame_rate()
        self.tile["rate"].configure(text=f"{r:.1f}/s" if r else "--")
        # staleness: the logger-side dead-transmitter concern, live on screen
        if self.bench:
            self.tile["stale"].configure(text="suspended", text_color=ORANGE)
        elif self.last_frame_wall is None:
            self.tile["stale"].configure(text="--", text_color=MUTED)
        elif (time.time() - self.last_frame_wall) > STALE_S:
            self.tile["stale"].configure(text="STALE", text_color=RED)
        else:
            self.tile["stale"].configure(text="live", text_color=GREEN)
        if "fault" in st:
            self.tile["fault"].configure(
                text=(st.get("status_name") or "FAULT") if st["fault"] else "ok",
                text_color=RED if st["fault"] else GREEN)
        self.cfg.configure(text=(
            f'Rate: {st.get("rate_ms","--")}ms    '
            f'Probe: {st.get("probe","--")}    '
            f'Cal: {"VALID" if st.get("cal_valid") else "NONE"}    '
            f'Faults: {st.get("faults","--")}'))

    # ---- Drawing ----------------------------------------------------------
    def _draw_gauge(self):
        c = self.gauge
        c.delete("all")
        w = c.winfo_width(); h = c.winfo_height()
        if w < 20 or h < 20:
            return
        pad = 24
        size = min(w, h) - 2 * pad
        x0 = (w - size) // 2; y0 = (h - size) // 2
        x1 = x0 + size; y1 = y0 + size
        c.create_arc(x0, y0, x1, y1, start=225, extent=-270, style="arc",
                     outline=PANEL2, width=18)
        if self.frac > 0:
            c.create_arc(x0, y0, x1, y1, start=225, extent=-270 * self.frac,
                         style="arc", outline=self.gcolor, width=18)
        cx = (x0 + x1) // 2; cy = (y0 + y1) // 2
        fsize = max(14, size // 7) if len(self.headline) <= 7 else max(12, size // 10)
        c.create_text(cx, cy - 6, text=self.headline, fill=TEXT,
                      font=("Segoe UI", fsize, "bold"))
        c.create_text(cx, cy + size // 5, text=self.unit, fill=MUTED,
                      font=("Segoe UI", 14))

    def _draw_chart(self):
        c = self.chart
        c.delete("all")
        w = c.winfo_width(); h = c.winfo_height()
        if w < 20 or h < 20:
            return
        for gy in (0.25, 0.5, 0.75):
            y = h * gy
            c.create_line(0, y, w, y, fill=PANEL2, width=1)
        data = list(self.history)
        if len(data) < 2:
            c.create_text(w // 2, h // 2,
                          text="waiting for packets... (connect to the stream line)",
                          fill=MUTED, font=("Segoe UI", 13))
            return
        n = len(data)
        pts = []
        for i, v in enumerate(data):
            x = w * i / (n - 1)
            y = h - clamp(v, 0, 1) * (h - 8) - 4
            pts += [x, y]
        c.create_line(*pts, fill=self.gcolor, width=2, smooth=True)
        lx, ly = pts[-2], pts[-1]
        c.create_oval(lx - 4, ly - 4, lx + 4, ly + 4, fill=self.gcolor, outline="")

    # ---- Misc -------------------------------------------------------------
    def _logline(self, s):
        self.log.insert("end", s + "\n")
        if int(self.log.index("end-1c").split(".")[0]) > 300:
            self.log.delete("1.0", "100.0")
        self.log.see("end")

    def _on_close(self):
        self._disconnect()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
