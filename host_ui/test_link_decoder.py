"""Unit tests for link_decoder.py — run: python3 -m unittest discover host_ui

Includes a cross-check against host_tests/golden_stream.csv, the byte-exact
wire capture emitted by the firmware protocol simulation (make -C host_tests),
so the C encoder and the Python decoder are tested against each other.
"""
import os
import csv
import unittest

from link_decoder import LinkDecoder, decode_code, checksum, SYNC


def packet(code):
    msb, lsb = (code >> 8) & 0xFF, code & 0xFF
    return bytes([SYNC, msb, lsb, checksum(msb, lsb)])


def stream(codes):
    return b"".join(packet(c) for c in codes)


def frames(events):
    return [p for k, p in events if k == "frame"]


def text(events):
    return b"".join(p for k, p in events if k == "text")


class TestDecodeCode(unittest.TestCase):
    def test_pressure(self):
        self.assertEqual(decode_code(0), ("pressure", 0.0))
        self.assertEqual(decode_code(1234), ("pressure", 123.4))
        self.assertEqual(decode_code(10000), ("pressure", 1000.0))

    def test_status(self):
        self.assertEqual(decode_code(0xFF01), ("status", "NO_READING"))
        self.assertEqual(decode_code(0xFF04), ("status", "ADC_STALL"))

    def test_invalid(self):
        self.assertEqual(decode_code(10001), ("invalid", 10001))
        self.assertEqual(decode_code(0xFF00), ("invalid", 0xFF00))
        self.assertEqual(decode_code(0xFFFF), ("invalid", 0xFFFF))


class TestCleanStream(unittest.TestCase):
    def test_basic(self):
        d = LinkDecoder()
        ev = d.feed(stream([0, 1, 5000, 10000, 0xFF02]))
        self.assertEqual(frames(ev), [0, 1, 5000, 10000, 0xFF02])
        self.assertEqual(d.checksum_errors, 0)
        self.assertEqual(d.text_bytes, 0)
        self.assertTrue(d.confident)

    def test_payload_0x7f_lsb(self):
        # code 127 -> 7F 00 7F 80: LSB is a literal sync byte
        d = LinkDecoder()
        ev = d.feed(stream([127, 5000]))
        self.assertEqual(frames(ev), [127, 5000])
        self.assertEqual(d.checksum_errors, 0)

    def test_payload_0x7f_checksum(self):
        # code 383 -> 7F 01 7F 7F: 0x7F in LSB AND checksum
        d = LinkDecoder()
        ev = d.feed(stream([383, 383, 2000]))
        self.assertEqual(frames(ev), [383, 383, 2000])
        self.assertEqual(d.checksum_errors, 0)

    def test_consecutive_0x7f(self):
        # code 0x7F7F -> 7F 7F 7F 01: four 0x7F-ish bytes in a row
        d = LinkDecoder()
        ev = d.feed(stream([0x7F7F, 42]))
        self.assertEqual(frames(ev), [0x7F7F, 42])

    def test_frame_rate_chunked_timestamps(self):
        # The GUI reads the serial port in ~0.1 s chunks, so 2-3 frames of a
        # 25/s stream share one timestamp (zero intervals). frame_rate() must
        # still report ~25/s, not the chunk cadence (~10/s).
        d = LinkDecoder()
        by_chunk = {}
        for j in range(250):                    # 250 frames = 10 s at 25/s
            by_chunk.setdefault(int(j * 0.040 / 0.1), []).append(j)
        for c in sorted(by_chunk):
            d.feed(stream([42] * len(by_chunk[c])), t=(c + 1) * 0.1)
        rate = d.frame_rate()
        self.assertIsNotNone(rate)
        self.assertAlmostEqual(rate, 25.0, delta=1.5)

    def test_chunked_feeds(self):
        data = stream(list(range(0, 2000, 7)))
        expect = list(range(0, 2000, 7))
        for chunk in (1, 3, 7, 100, len(data)):
            d = LinkDecoder()
            got = []
            for off in range(0, len(data), chunk):
                got += frames(d.feed(data[off:off + chunk]))
            self.assertEqual(got, expect, f"chunk={chunk}")
            self.assertEqual(d.checksum_errors, 0, f"chunk={chunk}")


class TestRecovery(unittest.TestCase):
    def test_mid_stream_join(self):
        data = stream([100, 200, 300, 400, 500])
        for off in range(1, 4):          # join mid-frame at any offset
            d = LinkDecoder()
            got = frames(d.feed(data[off:]))
            # the partial first frame is lost; everything after decodes
            self.assertEqual(got[-4:], [200, 300, 400, 500], f"off={off}")

    def test_one_dropped_byte(self):
        data = bytearray(stream([100, 200, 300, 400]))
        del data[6]                       # drop a byte inside frame 2
        d = LinkDecoder()
        got = frames(d.feed(bytes(data)))
        self.assertIn(100, got)
        self.assertEqual(got[-2:], [300, 400])

    def test_one_inserted_byte(self):
        data = bytearray(stream([100, 200, 300, 400]))
        data.insert(6, 0x55)              # inject noise inside frame 2
        d = LinkDecoder()
        got = frames(d.feed(bytes(data)))
        self.assertIn(100, got)
        self.assertEqual(got[-2:], [300, 400])

    def test_corrupt_each_position(self):
        for pos in range(4):              # corrupt sync, MSB, LSB, checksum
            data = bytearray(stream([100, 200, 300]))
            data[4 + pos] ^= 0xA5         # inside frame 2
            d = LinkDecoder()
            got = frames(d.feed(bytes(data)))
            self.assertEqual(got[0], 100, f"pos={pos}")
            self.assertEqual(got[-1], 300, f"pos={pos}")
            self.assertNotIn(200, got, f"pos={pos}: corrupt frame accepted")

    def test_truncated_tail_then_completion(self):
        data = stream([100, 200])
        d = LinkDecoder()
        got = frames(d.feed(data[:6]))    # frame 2 incomplete
        self.assertEqual(got, [100])
        got = frames(d.feed(data[6:]))    # completion arrives later
        self.assertEqual(got, [200])


class TestMixedText(unittest.TestCase):
    def test_console_text_interleaved(self):
        # Bench transitions: ASCII before and after packet bursts
        blob = b"Console LOCKED \r\n" + stream([1500, 1501]) + b"== banner ==\r\n"
        d = LinkDecoder()
        ev = d.feed(blob)
        self.assertEqual(frames(ev), [1500, 1501])
        self.assertIn(b"Console LOCKED", text(ev))
        self.assertIn(b"banner", text(ev))

    def test_pure_ascii_never_frames(self):
        # Printable ASCII cannot contain 0x7F -> zero false frames, ever.
        blob = ("STATUS\r\nProbeA: 2048 ProbeB: 2052 Avg: 2050\r\n"
                "Link: 0x04D2 LIVE mode=PKT pkts=123 aborts=0\r\n" * 50).encode()
        d = LinkDecoder()
        ev = d.feed(blob)
        self.assertEqual(frames(ev), [])
        self.assertEqual(d.frames_ok, 0)
        self.assertEqual(text(ev), blob)


class TestGoldenStream(unittest.TestCase):
    GOLDEN = os.path.join(os.path.dirname(__file__), "..", "host_tests",
                          "golden_stream.csv")

    @unittest.skipUnless(os.path.exists(GOLDEN),
                         "golden_stream.csv not built (make -C host_tests)")
    def test_firmware_capture_decodes(self):
        d = LinkDecoder()
        got = []
        with open(self.GOLDEN) as f:
            rows = list(csv.DictReader(f))
        # feed in bursts grouped by ~4 bytes with coarse timestamps
        for row in rows:
            got += frames(d.feed(bytes([int(row["byte"])]),
                                 t=float(row["time_ms"]) / 1000.0))
        self.assertGreaterEqual(len(got), 240)      # ~250 packets in 10 s
        self.assertEqual(d.checksum_errors, 0)
        self.assertEqual(d.text_bytes, 0)
        self.assertEqual(set(got), {1234})          # the simulation's code
        rate = d.frame_rate()
        self.assertIsNotNone(rate)
        self.assertAlmostEqual(rate, 25.0, delta=1.5)


if __name__ == "__main__":
    unittest.main()
