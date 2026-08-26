#!/usr/bin/env python3
"""Long-duration capture of the downhole link stream (24 h soak).

Records the RAW bytes off the wire plus arrival timestamps, and nothing else.
Decoding and scoring are soak_verify.py's job — a capture tool that also
interprets is a capture tool that can lose evidence when its interpretation
is wrong.

Wiring (see verification_guide.md):

    board TX P1.0 --+--> logger RX          (the real recording)
                    +--> this host's USB-serial RX   (passive tap)

The logger input is high-Z, so tapping in parallel is safe at 9600. The tap is
what makes verification possible regardless of what the logger can export.

Rules this tool obeys, and why:
  * It NEVER transmits. The link is one-way by design, the console is locked
    for the whole soak, and any byte sent toward the board would be console
    input that could unlock it and suspend the packet stream.
  * It never issues settings commands. RATE/THRESH/RANGE/PROBE/CAL STORE each
    write NVM (30k cycle endurance), and RATE in particular would rescale the
    profile and invalidate the reference.

Output: a rotating set of .bin files plus a sidecar .idx of
(wall_clock, monotonic, byte_offset) per read chunk, so soak_verify.py can put
a timestamp on any byte without this tool having to frame anything.

Usage:
    python soak_capture.py --port COM5 --out logs/soak_2026-08-26
    python soak_capture.py --port /dev/ttyUSB0 --out logs/run1 --hours 24
"""
import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install -r requirements.txt")

BAUD = 9600                 # link_protocol.md §1 — not the old 115200 console
ROTATE_BYTES = 32 << 20     # 32 MiB per chunk file
READ_CHUNK = 4096
# 4 bytes/packet at 25 packets/s = 100 B/s, so 24 h is only ~8.6 MB. Rotation
# is cheap insurance for a run that overshoots or gets left going.


class Capture:
    def __init__(self, out_prefix):
        self.prefix = out_prefix
        os.makedirs(os.path.dirname(os.path.abspath(out_prefix)) or ".",
                    exist_ok=True)
        self.part = 0
        self.total = 0
        self._open()

    def _open(self):
        self.path = "%s.%03d.bin" % (self.prefix, self.part)
        self.fh = open(self.path, "wb")
        self.idx = open("%s.%03d.idx" % (self.prefix, self.part), "w")
        self.idx.write("# wall_clock_epoch\tmonotonic\tbyte_offset_in_part\n")
        self.in_part = 0

    def write(self, data, t_wall, t_mono):
        if self.in_part >= ROTATE_BYTES:
            self.close()
            self.part += 1
            self._open()
        self.idx.write("%.6f\t%.6f\t%d\n" % (t_wall, t_mono, self.in_part))
        self.fh.write(data)
        self.in_part += len(data)
        self.total += len(data)

    def flush(self):
        self.fh.flush()
        self.idx.flush()
        os.fsync(self.fh.fileno())      # a 24 h run must survive a host crash
        os.fsync(self.idx.fileno())

    def close(self):
        try:
            self.flush()
        finally:
            self.fh.close()
            self.idx.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial port (COM5, /dev/ttyUSB0)")
    ap.add_argument("--out", required=True, help="output path prefix")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--hours", type=float, default=None,
                    help="stop after N hours (default: run until interrupted)")
    ap.add_argument("--quiet-warn", type=float, default=2.0,
                    help="warn after this many seconds with no bytes")
    args = ap.parse_args()

    deadline = None if args.hours is None else time.monotonic() + args.hours * 3600.0
    cap = Capture(args.out)
    ser = None
    last_rx = time.monotonic()
    last_report = last_rx
    quiet_reported = False
    reconnects = 0
    quiet_total = 0.0

    print("capturing %s @ %d 8N1 -> %s.NNN.bin   (Ctrl-C to stop)"
          % (args.port, args.baud, args.out))
    print("NOTE: this tool never transmits; keep the console LOCKED.")
    try:
        while deadline is None or time.monotonic() < deadline:
            # --- (re)connect ------------------------------------------------
            if ser is None:
                try:
                    ser = serial.Serial(args.port, args.baud, timeout=0.5)
                except Exception as e:                       # noqa: BLE001
                    # A 24 h run must survive a USB re-enumeration; the old
                    # GUI had no reconnect at all, which is why it could not
                    # be used for this.
                    print("  [%s] open failed (%s), retrying in 2 s"
                          % (time.strftime("%H:%M:%S"), e))
                    time.sleep(2.0)
                    continue
                reconnects += 1
                if reconnects > 1:
                    print("  [%s] reconnected (#%d)"
                          % (time.strftime("%H:%M:%S"), reconnects - 1))

            # --- read -------------------------------------------------------
            try:
                data = ser.read(READ_CHUNK)
            except Exception as e:                           # noqa: BLE001
                print("  [%s] read failed (%s), reconnecting"
                      % (time.strftime("%H:%M:%S"), e))
                try:
                    ser.close()
                except Exception:                            # noqa: BLE001
                    pass
                ser = None
                continue

            now = time.monotonic()
            if data:
                cap.write(data, time.time(), now)
                if quiet_reported:
                    print("  [%s] stream resumed after %.1f s quiet"
                          % (time.strftime("%H:%M:%S"), now - last_rx))
                    quiet_reported = False
                last_rx = now
            else:
                quiet = now - last_rx
                if quiet >= args.quiet_warn and not quiet_reported:
                    # Silence is the logger's dead-transmitter case
                    # (link_protocol.md §5) — worth stamping in the console
                    # log, but the capture itself is the evidence.
                    print("  [%s] NO DATA for %.1f s"
                          % (time.strftime("%H:%M:%S"), quiet))
                    quiet_reported = True
                    quiet_total += quiet

            # --- periodic progress -----------------------------------------
            if now - last_report >= 60.0:
                cap.flush()
                mb = cap.total / (1024.0 * 1024.0)
                pkts = cap.total / 4.0
                print("  [%s] %.2f MB  ~%d packets  parts=%d  reconnects=%d"
                      % (time.strftime("%H:%M:%S"), mb, int(pkts),
                         cap.part + 1, reconnects - 1))
                last_report = now
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:                                # noqa: BLE001
                pass
        cap.close()
        print("captured %d bytes (~%d packets) across %d part(s) -> %s.*.bin"
              % (cap.total, cap.total // 4, cap.part + 1, args.out))
        if reconnects > 1:
            print("WARNING: %d reconnect(s) — check the tap wiring before "
                  "trusting gap statistics." % (reconnects - 1))


if __name__ == "__main__":
    main()
