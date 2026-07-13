#!/usr/bin/env python3
"""
DeepProbe -- Downhole Pressure Probe Monitor (MVP dashboard)

A quick host-side GUI for the TLE9854 pressure-transmitter firmware. It speaks
the same bench UART protocol as the terminal (115200 8N1): on connect it sends
STATUS + AUTO, then parses the streaming telemetry lines into a live gauge,
strip chart, and stat tiles. Buttons replace the common terminal commands.

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

import serial
import serial.tools.list_ports

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
BAUD    = 115200
BAR_TO_PSI = 14.5037738   # display-only unit conversion (firmware is in bar)

# ---- Branding (change these two lines to rename the app) -------------------
APP_NAME    = "DeepProbe"
APP_TAGLINE = "Downhole Pressure Probe Monitor"

# ---- Telemetry parsing -----------------------------------------------------
# AUTO line, e.g.:  A:96 497mV  B:1 2mV  Avg:96  P:0.000bar  Out:0.499V CAL
RE_A     = re.compile(r"A:\s*(\d+)\s+(\d+)\s*mV")
RE_B     = re.compile(r"B:\s*(\d+)\s+(\d+)\s*mV")
RE_AVG   = re.compile(r"Avg:\s*(\d+)")
RE_P     = re.compile(r"P:\s*(uncal|[\d.]+)")
RE_OUT   = re.compile(r"Out:\s*([\d.]+)\s*V")
RE_TAG   = re.compile(r"\b(FLT|MAN|CAL|RAW)\b")
# STATUS lines
RE_SOUT  = re.compile(r"Output:\s*([\d.]+)\s*V\s+(AUTO|MANUAL)\s+Fault:\s*(YES|no)")
RE_RATE  = re.compile(r"Rate:\s*(\d+)\s*ms")
RE_THR   = re.compile(r"Thresh:\s*(\d+)")
RE_RANGE = re.compile(r"Range:\s*([\d.]+)\s*-\s*([\d.]+)\s*bar")
RE_CAL   = re.compile(r"Cal:\s*(NONE|VALID)")
RE_CALSO = re.compile(r"slope=([-\d.]+)\s+offset=([-\d.]+)")
RE_PROBE = re.compile(r"Probe:\s*(AVG|A|B)")


def is_telemetry(line):
    return ("Out:" in line) and bool(RE_TAG.search(line))


def parse_line(line, st):
    """Update state dict `st` in-place from one firmware line."""
    m = RE_A.search(line)
    if m: st["a"], st["a_mv"] = int(m.group(1)), int(m.group(2))
    m = RE_B.search(line)
    if m: st["b"], st["b_mv"] = int(m.group(1)), int(m.group(2))
    m = RE_AVG.search(line)
    if m: st["avg"] = int(m.group(1))
    m = RE_P.search(line)
    if m: st["p_bar"] = None if m.group(1) == "uncal" else float(m.group(1))
    m = RE_OUT.search(line)
    if m: st["out_v"] = float(m.group(1))
    m = RE_TAG.search(line)
    if m:
        st["mode"] = m.group(1)
        st["fault"] = (m.group(1) == "FLT")
    m = RE_SOUT.search(line)
    if m:
        st["out_v"] = float(m.group(1))
        st["manual"] = (m.group(2) == "MANUAL")
        st["fault"] = (m.group(3) == "YES")
    m = RE_RATE.search(line)
    if m: st["rate_ms"] = int(m.group(1))
    m = RE_THR.search(line)
    if m: st["thresh"] = int(m.group(1))
    m = RE_RANGE.search(line)
    if m: st["range_lo"], st["range_hi"] = float(m.group(1)), float(m.group(2))
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
    def __init__(self, port, rx_queue):
        super().__init__(daemon=True)
        self.port = port
        self.rx = rx_queue
        self.ser = None
        self._stop = threading.Event()
        self._wlock = threading.Lock()
        self.error = None

    def run(self):
        try:
            self.ser = serial.Serial(self.port, BAUD, timeout=0.2)
        except Exception as e:
            self.error = str(e)
            self.rx.put(("__err__", str(e)))
            return
        self.rx.put(("__open__", self.port))
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(256)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        s = line.decode("ascii", "replace").strip("\r\n ")
                        if s:
                            self.rx.put(("line", s))
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
            return
        try:
            with self._wlock:
                self.ser.write((cmd + "\r\n").encode("ascii"))
        except Exception as e:
            self.rx.put(("__err__", str(e)))

    def stop(self):
        self._stop.set()


# ---- App -------------------------------------------------------------------
class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        ctk.set_appearance_mode("dark")
        self.title(f"{APP_NAME} — {APP_TAGLINE}")
        self.geometry("1120x760")
        self.minsize(940, 640)
        self.configure(fg_color=BG)

        self.worker = None
        self.rx = queue.Queue()
        self.tele = {}
        self.history = deque(maxlen=300)     # headline value over time
        self.headline = "--"
        self.unit = ""
        self.frac = 0.0
        self.gcolor = ACCENT
        self._last_status = 0.0
        self.unit_pref = "bar"          # display unit: "bar" or "psi"

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
        self.grid_rowconfigure(5, weight=1)

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
        ctk.CTkLabel(gcard, text="LIVE", font=self.f_lbl, text_color=MUTED).pack(
            anchor="w", padx=16, pady=(12, 0))
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
        names = [("Probe A", "a"), ("Probe B", "b"), ("Avg", "avg"),
                 ("Output", "out"), ("Mode", "mode"), ("Fault", "fault")]
        for i, (label, key) in enumerate(names):
            tiles.grid_columnconfigure(i, weight=1)
            f = ctk.CTkFrame(tiles, fg_color=PANEL, corner_radius=10)
            f.grid(row=0, column=i, sticky="ew", padx=4)
            ctk.CTkLabel(f, text=label, font=self.f_lbl, text_color=MUTED).pack(
                pady=(8, 0))
            v = ctk.CTkLabel(f, text="--", font=self.f_val, text_color=TEXT)
            v.pack(pady=(0, 8))
            self.tile[key] = v

        # Config strip
        self.cfg = ctk.CTkLabel(self, text="Rate: --   Range: --   Probe: --   Cal: --",
                                font=self.f_lbl, text_color=MUTED)
        self.cfg.grid(row=3, column=0, sticky="w", padx=20, pady=(0, 4))

        # Quick buttons
        qb = ctk.CTkFrame(self, fg_color="transparent")
        qb.grid(row=4, column=0, sticky="ew", padx=12, pady=4)
        quick = [("Stream On/Off", "AUTO"), ("Refresh", "STATUS"),
                 ("Probe A", "PROBE A"), ("Probe AVG", "PROBE AVG"),
                 ("Output Auto", "OUTPUT AUTO"),
                 ("Fast 10Hz", "RATE 100"), ("Normal 1Hz", "RATE 1000")]
        for label, cmd in quick:
            ctk.CTkButton(qb, text=label, command=lambda c=cmd: self._send(c),
                          fg_color=PANEL2, hover_color=ACCENT, width=110,
                          font=self.f_lbl).pack(side="left", padx=4)

        # Raw command + log
        bottom = ctk.CTkFrame(self, fg_color=PANEL, corner_radius=12)
        bottom.grid(row=5, column=0, sticky="nsew", padx=12, pady=(6, 12))
        bottom.grid_columnconfigure(0, weight=1)
        bottom.grid_rowconfigure(1, weight=1)
        row = ctk.CTkFrame(bottom, fg_color="transparent")
        row.grid(row=0, column=0, sticky="ew", padx=10, pady=8)
        row.grid_columnconfigure(0, weight=1)
        self.entry = ctk.CTkEntry(row, placeholder_text="type a command (e.g. RANGE 0 600, CAL ARM)...",
                                  font=self.f_small)
        self.entry.grid(row=0, column=0, sticky="ew", padx=(0, 8))
        self.entry.bind("<Return>", lambda _e: self._send_entry())
        ctk.CTkButton(row, text="Send", width=80, fg_color=ACCENT,
                      command=self._send_entry).grid(row=0, column=1)
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
        self.worker = SerialWorker(port, self.rx)
        self.worker.start()
        self.btn_conn.configure(text="Disconnect", fg_color=RED)

    def _disconnect(self):
        if self.worker:
            self.worker.send("AUTO")   # try to stop the stream
            time.sleep(0.05)
            self.worker.stop()
            self.worker = None
        self.btn_conn.configure(text="Connect", fg_color=ACCENT)
        self.lbl_conn.configure(text="●  disconnected", text_color=MUTED)

    def _send(self, cmd):
        if self.worker:
            self.worker.send(cmd)
            self._logline(">> " + cmd)
        else:
            self._logline("! not connected")

    def _send_entry(self):
        cmd = self.entry.get().strip()
        if cmd:
            self._send(cmd)
            self.entry.delete(0, "end")

    # ---- Main poll loop ---------------------------------------------------
    def _poll(self):
        new_auto = False
        try:
            for _ in range(200):
                kind, payload = self.rx.get_nowait()
                if kind == "__open__":
                    self.lbl_conn.configure(text="●  connected " + payload,
                                            text_color=GREEN)
                    self._logline("-- connected " + payload)
                    self.worker.send("STATUS")
                    self.worker.send("AUTO")
                elif kind == "__err__":
                    self.lbl_conn.configure(text="●  error", text_color=RED)
                    self._logline("! " + payload)
                    self._disconnect()
                elif kind == "line":
                    self._logline(payload)
                    parse_line(payload, self.tele)
                    if is_telemetry(payload):
                        new_auto = True
        except queue.Empty:
            pass

        # periodically refresh full STATUS (rate/range/probe/cal)
        if self.worker and (time.time() - self._last_status) > 4.0:
            self._last_status = time.time()
            self.worker.send("STATUS")

        if new_auto:
            self._update_headline()
            self.history.append(self.frac)
        self._update_tiles()
        self._draw_gauge()
        self._draw_chart()
        self.after(50, self._poll)

    def _set_unit(self, val):
        self.unit_pref = val
        self._update_headline()
        self._update_tiles()
        self._draw_gauge()

    def _update_headline(self):
        st = self.tele
        p = st.get("p_bar")
        if p is not None:
            if self.unit_pref == "psi":
                self.headline = f"{p * BAR_TO_PSI:.0f}"
                self.unit = "psi"
            else:
                self.headline = f"{p:.1f}"
                self.unit = "bar"
            lo = st.get("range_lo", 0.0); hi = st.get("range_hi", 1000.0)
            self.frac = clamp((p - lo) / (hi - lo + 1e-6), 0, 1)
        else:
            out = st.get("out_v", 0.5)
            self.headline = f"{out:.2f}"
            self.unit = "V out (uncal)"
            self.frac = clamp((out - 0.5) / 4.0, 0, 1)
        mode = st.get("mode", "RAW")
        self.gcolor = {"FLT": RED, "MAN": ORANGE, "CAL": GREEN}.get(mode, ACCENT)

    def _update_tiles(self):
        st = self.tele
        if "a" in st:   self.tile["a"].configure(text=f'{st["a"]}  ({st.get("a_mv","?")}mV)')
        if "b" in st:   self.tile["b"].configure(text=f'{st["b"]}  ({st.get("b_mv","?")}mV)')
        if "avg" in st: self.tile["avg"].configure(text=str(st["avg"]))
        if "out_v" in st: self.tile["out"].configure(text=f'{st["out_v"]:.2f} V')
        if "mode" in st:
            col = {"FLT": RED, "MAN": ORANGE, "CAL": GREEN}.get(st["mode"], ACCENT)
            self.tile["mode"].configure(text=st["mode"], text_color=col)
        if "fault" in st:
            self.tile["fault"].configure(
                text="FAULT" if st["fault"] else "ok",
                text_color=RED if st["fault"] else GREEN)
        lo = st.get("range_lo"); hi = st.get("range_hi")
        if lo is not None and hi is not None:
            if self.unit_pref == "psi":
                rng = f"{lo * BAR_TO_PSI:.0f}-{hi * BAR_TO_PSI:.0f} psi"
            else:
                rng = f"{lo:.0f}-{hi:.0f} bar"
        else:
            rng = "--"
        self.cfg.configure(text=(
            f'Rate: {st.get("rate_ms","--")}ms    '
            f'Range: {rng}    '
            f'Probe: {st.get("probe","--")}    '
            f'Cal: {"VALID" if st.get("cal_valid") else "NONE"}'))

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
        c.create_text(cx, cy - 6, text=self.headline, fill=TEXT,
                      font=("Segoe UI", max(20, size // 7), "bold"))
        c.create_text(cx, cy + size // 5, text=self.unit, fill=MUTED,
                      font=("Segoe UI", 14))

    def _draw_chart(self):
        c = self.chart
        c.delete("all")
        w = c.winfo_width(); h = c.winfo_height()
        if w < 20 or h < 20:
            return
        # gridlines
        for gy in (0.25, 0.5, 0.75):
            y = h * gy
            c.create_line(0, y, w, y, fill=PANEL2, width=1)
        data = list(self.history)
        if len(data) < 2:
            c.create_text(w // 2, h // 2, text="waiting for stream...  (Stream On/Off)",
                          fill=MUTED, font=("Segoe UI", 13))
            return
        n = len(data)
        # data is fraction 0..1 of the headline scale
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
        # keep the log bounded
        if int(self.log.index("end-1c").split(".")[0]) > 300:
            self.log.delete("1.0", "100.0")
        self.log.see("end")

    def _on_close(self):
        self._disconnect()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
