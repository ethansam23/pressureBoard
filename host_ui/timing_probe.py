"""Passive wire-timing probe for the downhole packet stream.

Byte-timestamped statistics (period / sync-gap / idle / packet length) from
a plain USB-UART adapter, with an honest self-assessment of measurement
quality: the three data bytes are back-to-back on the wire (exactly one
byte-time apart, 1.042 ms at 9600 8N1), so whatever spread the host observes
on THOSE deltas is pure USB/driver smear. That noise floor decides which
statistics are trustworthy. This is soft evidence for the bench record --
it supplements, never replaces, the logic-analyzer rows of Test 2.

READ-ONLY by construction: opens the port with TX never written and
DTR/RTS deasserted.

Usage:
  python timing_probe.py COM5 [--seconds 30] [--dump] [--csv out.csv]
  python timing_probe.py --replay ../host_tests/golden_stream.csv  (self-test)
"""
import argparse
import csv
import sys
import time

from link_decoder import SYNC, checksum, decode_code

BYTE_MS = 10000.0 / 9600.0          # one 8N1 byte on the wire: 1.0417 ms

# Test 2 limits (scope column; idle floor per the 2026-07-13 audit note)
LIMITS = {
    "period": (39.0, 41.0),
    "gap": (2.9, 5.0),
    "idle": (21.8, None),
    "total": (None, 9.5),
}


def walk_frames(samples):
    """samples: list of (t_ms, byte). Returns (packets, chk_errors, text_bytes).

    Each packet is a dict of delivery timestamps for sync/msb/lsb/chk plus the
    decoded code. Same sliding-recovery framing rule as link_decoder."""
    packets, chk_errors, text_bytes = [], 0, 0
    i, n = 0, len(samples)
    while i < n:
        t, b = samples[i]
        if b != SYNC:
            text_bytes += 1
            i += 1
            continue
        if i + 3 >= n:
            break
        (t1, msb), (t2, lsb), (t3, chk) = samples[i + 1], samples[i + 2], samples[i + 3]
        if chk == checksum(msb, lsb):
            packets.append({"t_sync": t, "t_msb": t1, "t_lsb": t2,
                            "t_chk": t3, "code": (msb << 8) | lsb})
            i += 4
        else:
            chk_errors += 1
            text_bytes += 1
            i += 1
    return packets, chk_errors, text_bytes


def stats(xs):
    if not xs:
        return None
    s = sorted(xs)
    return {"n": len(s), "mean": sum(s) / len(s), "min": s[0], "max": s[-1],
            "p95": s[min(len(s) - 1, int(round(0.95 * (len(s) - 1))))]}


def analyze(samples, dump=False):
    packets, chk_errors, text_bytes = walk_frames(samples)
    if dump:
        for p in packets:
            kind, val = decode_code(p["code"])
            print("  %10.3f ms  %04X  %s %s"
                  % (p["t_sync"], p["code"], kind, val))
    if len(packets) < 10:
        print("Only %d valid packets -- nothing to analyze." % len(packets))
        return None

    # In-band ruler: data-byte deltas are exactly BYTE_MS on the wire.
    ruler = []
    for p in packets:
        ruler.append(abs((p["t_lsb"] - p["t_msb"]) - BYTE_MS))
        ruler.append(abs((p["t_chk"] - p["t_lsb"]) - BYTE_MS))
    floor = stats(ruler)

    periods = [b["t_sync"] - a["t_sync"] for a, b in zip(packets, packets[1:])]
    gaps = [p["t_msb"] - p["t_sync"] - BYTE_MS for p in packets]
    idles = [b["t_sync"] - a["t_chk"] - BYTE_MS for a, b in zip(packets, packets[1:])]
    totals = [p["t_chk"] - p["t_sync"] + BYTE_MS for p in packets]
    # Long-baseline mean period: immune to per-byte smear.
    mean_period = (packets[-1]["t_sync"] - packets[0]["t_sync"]) / (len(packets) - 1)

    return {"packets": packets, "chk_errors": chk_errors, "text_bytes": text_bytes,
            "floor": floor, "mean_period": mean_period,
            "period": stats(periods), "gap": stats(gaps),
            "idle": stats(idles), "total": stats(totals)}


def verdict(name, st, reliable):
    lo, hi = LIMITS[name]
    ok = (lo is None or st["min"] >= lo) and (hi is None or st["max"] <= hi)
    lim = "%s..%s" % ("" if lo is None else lo, "" if hi is None else hi)
    flag = ("within limits" if ok else "OUTSIDE LIMITS") if reliable \
        else "unreliable at this noise floor -- LA required"
    return "  %-6s  n=%-5d mean=%7.3f  min=%7.3f  max=%7.3f  p95=%7.3f  [%s]  %s" \
        % (name, st["n"], st["mean"], st["min"], st["max"], st["p95"], lim, flag)


def report(r):
    f = r["floor"]
    print("\nPackets: %d   checksum errors: %d   non-packet bytes: %d"
          % (len(r["packets"]), r["chk_errors"], r["text_bytes"]))
    print("Timestamp noise floor (data-byte ruler, wire truth = %.3f ms):" % BYTE_MS)
    print("  |delta - %.3f| mean=%.3f  p95=%.3f  max=%.3f ms"
          % (BYTE_MS, f["mean"], f["p95"], f["max"]))
    if f["p95"] <= 0.3:
        grade, reliable = "GOOD -- per-packet numbers meaningful", True
    elif f["p95"] <= 1.0:
        grade, reliable = "MARGINAL -- treat min/max with suspicion", True
    else:
        grade, reliable = ("POOR -- adapter batches bytes; gap/idle/total "
                           "say nothing about the wire"), False
    print("  => timestamp quality: %s" % grade)
    print("\nMean period (long baseline, smear-immune): %.4f ms  "
          "[39..41 => %s]" % (r["mean_period"],
                              "OK" if 39.0 <= r["mean_period"] <= 41.0 else "OUT"))
    print("Per-packet statistics (ms):")
    for name in ("period", "gap", "idle", "total"):
        print(verdict(name, r[name], reliable))
    print("\nNOTE: soft evidence only -- Test 2 sign-off still requires the "
          "logic analyzer.\n")


def capture_serial(port, seconds):
    import serial
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 9600, 0.05
    ser.dtr = False          # deasserted before open: provably passive
    ser.rts = False
    ser.open()
    print("Capturing %ds on %s (read-only)..." % (seconds, port))
    samples, t0 = [], time.perf_counter()
    try:
        while time.perf_counter() - t0 < seconds:
            b = ser.read(1)
            if b:
                samples.append(((time.perf_counter() - t0) * 1000.0, b[0]))
    finally:
        ser.close()
    return samples


def load_replay(path):
    with open(path, newline="") as fh:
        return [(float(row["time_ms"]), int(row["byte"]))
                for row in csv.DictReader(fh)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?", help="COM port, e.g. COM5")
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--dump", action="store_true", help="print every decoded frame")
    ap.add_argument("--csv", help="write per-byte (t_ms,byte) capture to this file")
    ap.add_argument("--replay", help="analyze a per-byte CSV (e.g. golden_stream.csv)")
    args = ap.parse_args()

    if args.replay:
        samples = load_replay(args.replay)
    elif args.port:
        samples = capture_serial(args.port, args.seconds)
    else:
        ap.error("give a COM port or --replay")

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["time_ms", "byte"])
            w.writerows(samples)
        print("Raw capture -> %s" % args.csv)

    r = analyze(samples, dump=args.dump)
    if r:
        report(r)
        bad = (r["chk_errors"] or r["text_bytes"])
        sys.exit(1 if bad else 0)
    sys.exit(2)


if __name__ == "__main__":
    main()
