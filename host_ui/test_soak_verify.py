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
PACKET_MS = 110
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


BEACON_CODE = 10000          # app_config.h SIM_AUTOSTART_BEACON_CODE
BEACON_S = 30                # app_config.h SIM_AUTOSTART_BEACON_MS
NO_READING = 0xFF01


def synth(rows, prefix, defect=None, at=None, beacon=False, reset_at=None):
    """Write a wire capture for `rows`.

    beacon   -- prepend the autostart boot preamble (NO_READING then the 30 s
                full-scale start beacon), as an APP_SIM_AUTOSTART build emits.
    reset_at -- reboot at this reference index: a second boot preamble, then
                the profile again from the top."""
    blob = bytearray()
    stamps = []
    t = 0.0

    def emit(code, n):
        nonlocal t
        msb, lsb = (code >> 8) & 0xFF, code & 0xFF
        chk = (~(msb + lsb)) & 0xFF
        for _ in range(n):
            stamps.append((len(blob), t))
            blob.extend(bytes([0x7F, msb, lsb, chk]))
            t += PACKET_MS / 1000.0

    def boot():
        emit(NO_READING, 10)                       # boot fail-safe
        emit(BEACON_CODE, BEACON_S * PKTS_PER_REFRESH)

    if beacon:
        boot()
    for index, code, _seg in rows:
        n = PKTS_PER_REFRESH
        if defect == "drop" and index == at:
            n = 0                                   # whole refresh lost
        if defect == "short" and index == at:
            n = 3                                   # most of a refresh lost
        emit(code, n)
        if defect == "corrupt" and index == at:
            blob[-1] ^= 0xFF                        # break the checksum
        if reset_at is not None and index == reset_at:
            boot()
            for _i, c2, _s2 in rows:
                emit(c2, PKTS_PER_REFRESH)
            break
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
        # The ladder's whole point: every ramp measured, none out of
        # tolerance. This asserted "worst |error| = 0 ms" while the period
        # was 40 ms, which divided the 1000 ms rate exactly (25
        # packets/refresh). At 110 ms it is 9.09, so packet-counted
        # durations quantize and every ramp carries a systematic ~1%
        # error. Assert the property the ladder actually gates on.
        self.assertIn("20 ramp(s) measured", out)
        self.assertNotIn("outside the duration tolerance", out)
        self.assertIn("packets/refresh", out)

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


@unittest.skipUnless(os.path.exists(HARNESS),
                     "build host_tests first: make -C host_tests")
class StartBeaconTest(unittest.TestCase):
    """The autostart beacon marks the start of a run AND detects reboots."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="beacon")
        cls.ref = os.path.join(cls.tmp, "ref.csv")
        cls.rows = emit_reference(cls.ref)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_leading_beacon_skipped_and_run_still_verifies(self):
        prefix = synth(self.rows, os.path.join(self.tmp, "beacon"), beacon=True)
        rc, out = run_verify(prefix, self.ref)
        self.assertEqual(rc, 0, out)
        self.assertIn("packets at full scale", out)
        self.assertIn("resets       : none", out)
        # Alignment must still land on the profile, not be thrown by the
        # boot preamble sitting in front of it.
        self.assertIn("aligned at reference run 0", out)

    def test_second_beacon_is_reported_as_a_reset(self):
        at = self.rows[1200][0]
        prefix = synth(self.rows, os.path.join(self.tmp, "reset"),
                       beacon=True, reset_at=at)
        rc, out = run_verify(prefix, self.ref)
        self.assertEqual(rc, 1, out)
        self.assertIn("BOARD RESET", out)
        self.assertIn("board reset(s)", out)
        # Located, not merely detected: 10 boot packets + the beacon +
        # (at+1) refreshes of profile, then the second boot's 10 packets.
        expect_pkt = 10 + BEACON_S * PKTS_PER_REFRESH + (at + 1) * PKTS_PER_REFRESH + 10
        self.assertIn("reset at packet %d" % expect_pkt, out)

    def test_profile_holds_at_full_scale_are_not_beacons(self):
        """Phase A parks at 10000 for 300 s and tier 5 touches it too.

        Only the beacon is followed by the status block, so those holds must
        not be misread as reboots. Asserted against the real profile rather
        than a contrived stream, since that adjacency is the whole rule."""
        ref_full = os.path.join(self.tmp, "ref_full.csv")
        rows_full = emit_reference(ref_full, RATE_MS, 0)     # phase FULL
        runs_at_max = sum(1 for i, (_i, c, _s) in enumerate(rows_full)
                          if c == BEACON_CODE
                          and (i == 0 or rows_full[i - 1][1] != BEACON_CODE))
        self.assertGreater(runs_at_max, 1,
                           "profile should contain several full-scale holds")
        prefix = synth(rows_full, os.path.join(self.tmp, "full_nobeacon"))
        rc, out = run_verify(prefix, ref_full)
        self.assertEqual(rc, 0, out)
        self.assertIn("none seen", out)          # zero false positives


class ArmingTest(unittest.TestCase):
    """--arm is the only path that may transmit, and it must fail loudly."""

    class FakeBoard:
        """Answers as app/uart_cmd.c does, so the parsing is really exercised."""

        def __init__(self, rate=1000, has_sim=True, console=True):
            self.rate, self.has_sim, self.console = rate, has_sim, console
            self.written, self._out = [], b""

        def reset_input_buffer(self):
            pass

        def flush(self):
            pass

        def write(self, data):
            cmd = data.decode().strip()
            self.written.append(cmd)
            u = cmd.upper()
            if u == "CONSOLE UNLOCK":
                self._out = (b"Console UNLOCKED - packet stream SUSPENDED\r\n"
                             if self.console else b"")
            elif u.startswith("SIM PHASE"):
                self._out = (b"Sim phase=FULL, index reset to 0\r\n"
                             if self.has_sim else b"ERR: unknown\r\n")
            elif u.startswith("SIM ") and "STATUS" not in u:
                self._out = (b"Sim BAR - SYNTHETIC pressure\r\n"
                             if self.has_sim else b"ERR: unknown\r\n")
            elif u == "SIM STATUS":
                self._out = ("Sim: BAR  phase=FULL  idx=0/86400  rate=%dms\r\n"
                             % self.rate).encode()
            elif u == "CONSOLE LOCK":
                self._out = b"Console LOCKED - stream resuming\r\n"
            return len(data)

        def read(self, n=1):
            out, self._out = self._out[:n], self._out[n:]
            return out

    def _arm(self, board, sim="BAR", phase="FULL", expect_rate=1000):
        from soak_capture import Armer
        a = Armer(board, sim, phase, expect_rate)
        return a.run(), a

    def test_healthy_board_arms_and_latches(self):
        ok, a = self._arm(self.FakeBoard())
        self.assertTrue(ok)
        self.assertTrue(a.done, "TX must latch off after arming")

    def test_command_sequence_and_order(self):
        board = self.FakeBoard()
        self._arm(board, sim="COUNTS", phase="B")
        self.assertEqual(board.written,
                         ["CONSOLE UNLOCK", "SIM PHASE B", "SIM COUNTS",
                          "SIM STATUS", "CONSOLE LOCK"])

    def test_rate_mismatch_aborts(self):
        """A wrong RATE invalidates the reference — fail before the run."""
        ok, a = self._arm(self.FakeBoard(rate=100), expect_rate=1000)
        self.assertFalse(ok)
        self.assertFalse(a.done)

    def test_sim_not_compiled_in_aborts(self):
        ok, _ = self._arm(self.FakeBoard(has_sim=False))
        self.assertFalse(ok)

    def test_production_build_without_console_aborts(self):
        ok, _ = self._arm(self.FakeBoard(console=False))
        self.assertFalse(ok)

    def test_arming_is_the_only_code_that_writes_to_the_port(self):
        """Structural invariant: without --arm the tool cannot transmit.

        Checked on the AST rather than by reading the docstring, so the
        guarantee survives future edits."""
        import ast
        src = open(os.path.join(HERE, "soak_capture.py")).read()
        tree = ast.parse(src)
        offenders = []
        for node in ast.walk(tree):
            if not isinstance(node, ast.ClassDef):
                continue
            for sub in ast.walk(node):
                if (isinstance(sub, ast.Call)
                        and isinstance(sub.func, ast.Attribute)
                        and sub.func.attr == "write"
                        and isinstance(sub.func.value, ast.Attribute)
                        and sub.func.value.attr == "ser"
                        and node.name != "Armer"):
                    offenders.append("%s:%d" % (node.name, sub.lineno))
        # module-level (outside any class) serial writes are offenders too
        classes = [n for n in ast.walk(tree) if isinstance(n, ast.ClassDef)]
        in_class = {id(x) for c in classes for x in ast.walk(c)}
        for node in ast.walk(tree):
            if (isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "write"
                    and isinstance(node.func.value, ast.Attribute)
                    and node.func.value.attr == "ser"
                    and id(node) not in in_class):
                offenders.append("module:%d" % node.lineno)
        self.assertEqual(offenders, [],
                         "serial write outside Armer: %s" % offenders)


if __name__ == "__main__":
    unittest.main()
