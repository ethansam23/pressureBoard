"""Unit tests for soak_verify.py — run: python3 -m unittest discover host_ui

Builds synthetic captures from a reference stream and checks that the verifier
PASSES a clean one and FAILS each defect it exists to catch. A verifier that
has only ever been run on good data is not evidence of anything.

The reference is produced by the firmware's own generator:
    make -C host_tests
    ./host_tests/test_link_frame --emit-ref <csv> <rate_ms> <phase>
These tests skip if that binary has not been built.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HARNESS = os.path.join(ROOT, "host_tests", "test_link_frame")
VERIFY = os.path.join(HERE, "soak_verify.py")

RATE_MS = 1000
PACKET_MS = 40
PKTS_PER_REFRESH = RATE_MS // PACKET_MS      # 25


def emit_reference(path, rate=RATE_MS, phase=2):
    subprocess.check_output([HARNESS, "--emit-ref", path, str(rate), str(phase)])
    rows = []
    with open(path) as fh:
        fh.readline()
        for line in fh:
            i, c, s = line.split(",")
            rows.append((int(i), int(c), int(s)))
    return rows


def synth(rows, prefix, defect=None, at=None):
    """Write a wire capture for `rows`, optionally with one injected defect."""
    blob = bytearray()
    stamps = []
    t = 0.0
    for index, code, _seg in rows:
        n = PKTS_PER_REFRESH
        if defect == "drop" and index == at:
            n = 0                                   # whole refresh lost
        if defect == "short" and index == at:
            n = 3                                   # most of a refresh lost
        for _ in range(n):
            msb, lsb = (code >> 8) & 0xFF, code & 0xFF
            chk = (~(msb + lsb)) & 0xFF
            stamps.append((len(blob), t))
            blob.extend(bytes([0x7F, msb, lsb, chk]))
            t += PACKET_MS / 1000.0
        if defect == "corrupt" and index == at:
            blob[-1] ^= 0xFF                        # break the checksum
    with open(prefix + ".000.bin", "wb") as fh:
        fh.write(bytes(blob))
    with open(prefix + ".000.idx", "w") as fh:
        fh.write("# wall\tmono\toff\n")
        for off, tt in stamps:
            fh.write("%.6f\t%.6f\t%d\n" % (tt, tt, off))
    return prefix


def run_verify(prefix, ref):
    p = subprocess.run([sys.executable, VERIFY, "--capture", prefix,
                        "--reference", ref, "--rate", str(RATE_MS)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


@unittest.skipUnless(os.path.exists(HARNESS),
                     "build host_tests first: make -C host_tests")
class SoakVerifyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="soakverify")
        cls.ref = os.path.join(cls.tmp, "ref.csv")
        cls.rows = emit_reference(cls.ref)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _cap(self, name, defect=None, at=None):
        return synth(self.rows, os.path.join(self.tmp, name), defect, at)

    def test_clean_capture_passes(self):
        rc, out = run_verify(self._cap("clean"), self.ref)
        self.assertEqual(rc, 0, out)
        self.assertIn("RESULT: PASS", out)
        # The ladder's whole point: every ramp measured, none out of tolerance.
        self.assertIn("worst |error| = 0 ms", out)

    def test_status_block_is_not_mistaken_for_a_reset(self):
        """The profile emits NO_READING once per cycle by design."""
        rc, out = run_verify(self._cap("clean2"), self.ref)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("BOARD RESET", out)

    def test_dropped_refresh_is_caught_and_located(self):
        at = self.rows[1500][0]
        rc, out = run_verify(self._cap("drop", "drop", at), self.ref)
        self.assertEqual(rc, 1, out)
        self.assertIn("value divergence at reference index %d" % at, out)

    def test_short_run_is_caught(self):
        """A partly-lost refresh keeps the right value but too few packets."""
        at = self.rows[1500][0]
        rc, out = run_verify(self._cap("short", "short", at), self.ref)
        self.assertEqual(rc, 1, out)
        self.assertIn("under half the expected packet count", out)

    def test_checksum_corruption_is_caught(self):
        at = self.rows[2000][0]
        rc, out = run_verify(self._cap("corrupt", "corrupt", at), self.ref)
        self.assertEqual(rc, 1, out)
        self.assertIn("checksum error", out)

    def test_full_range_and_resolution_reported(self):
        """Phase A must show all 10,001 codes; the ladder alone must not."""
        ref_a = os.path.join(self.tmp, "ref_a.csv")
        rows_a = emit_reference(ref_a, RATE_MS, 1)
        prefix = synth(rows_a, os.path.join(self.tmp, "phase_a"))
        rc, out = run_verify(prefix, ref_a)
        self.assertEqual(rc, 0, out)
        self.assertIn("distinct pressure codes seen : 10001", out)
        self.assertIn("coverage                     : 100.0000%", out)


if __name__ == "__main__":
    unittest.main()
