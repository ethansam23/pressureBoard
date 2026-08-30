# Link Verification — Test Plan

**Run order and go/no-go gates**, from a bare bench to a live 24-hour stream
into the replica logger. `verification_guide.md` is the *catalogue* — what each
test does and how to run it. This is the *campaign* — what order, what has to
pass before what, and what to do when something fails.

The organising principle: **nothing reaches the replica logger until the board
has proven itself alone.** Each phase gates the next. If a phase fails, you fix
it there — you do not carry a known defect forward, because a failure that
appears with the logger attached is far more expensive to diagnose than the
same failure on a bench adapter.

---

## What this campaign does and does not cover

**In scope — the link path.** Does the firmware emit the right 16-bit codes,
with the right packet timing, continuously, for 24 hours.

**Out of scope — the transducer and calibration path.** Deliberately. The
synthetic profile (`SIM BAR`) substitutes for the probes entirely, which is
what makes this testable now.

That has a useful consequence: **Phases 0–7 need no pressure source, no gauge,
and no calibration.** Tests 9 and 11 in the guide (ENOB noise capture,
calibration workflow) are a separate track that can run in parallel or later.
The only thing that needs a stored calibration is `SIM COUNTS` (Phase 8), which
re-introduces the processing maths once the link itself is proven.

---

## Build targets

Three Keil targets are needed. **The `.uvprojx` is yours to edit** — CLAUDE.md
forbids me touching it, so these have to be created by hand:

| Target | Defines | Used in |
|---|---|---|
| **Production** | `LINK_CONSOLE_EN=0` (sim off by default) | Phase 9 only |
| **Bench** | `APP_ENABLE_SIM=1` | Phases 1–5, 8 |
| **Bench-autostart** | `APP_ENABLE_SIM=1`, `APP_SIM_AUTOSTART=1` | Phases 6–7 |

`APP_ENABLE_SIM` defaults to 0 and `APP_SIM_AUTOSTART` is rejected at compile
time without it, so a mis-set target fails loudly rather than shipping a
synthetic value.

---

## Phase 0 — Desk gates (no hardware, minutes)

Everything downstream assumes these. Run them before touching the board.

| Check | Command | Gate |
|---|---|---|
| Protocol core + profile | `make -C host_tests` | ALL TESTS PASSED + `refs/ up to date` |
| Sanitisers | `make -C host_tests ubsan` | ALL TESTS PASSED |
| Host tooling | `python -m unittest discover host_ui` | 32 tests OK |

> **The two `make` lines need gcc, which the bench PC probably does not have.**
> They are a developer gate, not a bench gate — run them wherever the firmware
> is edited (WSL, MSYS2/MinGW, or CI). The bench needs **only Python**: the
> reference streams are committed in `host_tests/refs/`, so no compiler sits
> between you and a test run. `make` regenerates them into a temp directory and
> fails on any difference, so they cannot silently drift from the code.
| Production build | Keil, Production target | 0 warnings; confirm no sim code in the map file |
| Bench build | Keil, Bench target | 0 warnings |
| Autostart build | Keil, Bench-autostart target | 0 warnings |

> **These three firmware targets have never been compiled by anything.** There
> is no ARM toolchain in the environment the firmware was written in, so
> `main.c`, `uart_cmd.c` and `link_tx.c` have only been preprocessor-checked.
> Expect to fix compile errors here. This is the single most likely place for
> the campaign to stall on day one — budget for it.

**Gate:** all six green. Do not flash anything until they are.

---

## Phase 1 — Bench bring-up (adapter only, ~1 h)

Rig per **Test 15a**. One 5 V USB-UART adapter, external pull-up on P1.0,
J-Link **detached**, board on a bench supply.

| Step | Test | What it proves |
|---|---|---|
| 1.1 | Test 1 | Packets-only boot; no ASCII at boot; fail-safe `NO_READING` |
| 1.2 | **Test 2** ★ | Wire timing on a scope — sync gap, packet ≤ 9.17 ms, idle ≥ 22 ms, 40 ms period |
| 1.3 | Test 3 | Console mutual exclusion — packets and text never interleave |
| 1.4 | Test 5 | `LINKTEST` payload-sacred vectors — `0x7F` transmitted verbatim |
| 1.5 | Test 12 | 10-minute watchdog soak, standalone |
| 1.6 | Test 10 | First flash only — both NVM magics were bumped, so settings and calibration are intentionally reset. Re-enter settings before Phase 4's persistence check |

**Test 2 is the one that cannot be skipped or deferred.** It is the only
instrument that can confirm the timing the logger's framing depends on, and it
resolves the TI-timestamp hypothesis in `link_protocol.md` §6, which is
currently an unproven assumption underneath every timing number in the repo.

**Record:** measured sync gap, total packet time, idle, period — nominal *and*
worst observed. These are the numbers the logger designer's gates are answered
against.

**Gate:** wire timing inside spec, no resets in the 10-minute soak.
**On failure:** stop. Timing defects invalidate everything downstream, and no
amount of soak time will diagnose them without the scope.

---

## Phase 2 — Prove the measurement chain (adapter only, ~1.5 h)

Before spending a day on a soak, prove the tooling can actually see a defect.

Needs only Python and the UART dongle — no compiler, no logger, no pressure
source. From `host_ui/`:

```
pip install -r requirements.txt
python -m serial.tools.list_ports -v          # find the board's COM port

# 2.1 — arm the board and capture 10 minutes
python soak_capture.py --port COM5 --out logs/smoke --arm --sim BAR --phase B

# 2.2 — score it against the committed reference
python soak_verify.py --capture logs/smoke ^
       --reference ../host_tests/refs/phaseB_rate1000.csv --rate 1000

# 2.3 — Test B: the same, run for a full 1 h ladder cycle
python soak_capture.py --port COM5 --out logs/ladder --arm --sim BAR --phase B --hours 1
python soak_verify.py --capture logs/ladder ^
       --reference ../host_tests/refs/phaseB_rate1000.csv --rate 1000
```

| Step | Gate |
|---|---|
| 2.1 | Arming prints each step; preflight reports ~25 packets/s |
| 2.2 | `RESULT: PASS` |
| 2.3 | All 20 ramps `ok`; worst \|error\| recorded |

**Record:** worst ramp timing error, in ms, across all 20 ramps and five window
lengths (5 min down to 10 s).

**Gate:** `RESULT: PASS` with every ramp in tolerance.
**On failure:** a systematic error across *all* ramps points at the tooling or
the RATE; an error on *some* points at the firmware. The per-tier breakdown in
the report tells you which.

---

## Phase 3 — Resolution and full range (adapter only, 5 h 43 m)

**Test A.** The only test that proves all 10,001 deci-bar codes encode and
transmit. Runs unattended — start it and leave.

**Gate:** `coverage : 100.0000%`, zero checksum errors, zero gaps > 75 ms.

This also sweeps every payload-sacred case organically: the 39 codes whose LSB
is `0x7F` (127, 383, 639 … 9855), the 39 whose checksum is `0x7F` (128, 383,
638 … 9818), and the single code carrying it in both — 383, the exact vector
`link_protocol.md` calls out. It subsumes Test 5's four hand-picked vectors
with exhaustive coverage.

**On failure:** a *missing* code is an encoder defect; a *corrupted* one is a
wire or tap defect. The report names which codes and where.

---

## Phase 4 — Fault paths and edges (adapter only, ~1 h)

The synthetic profile proves fault codes *reach* the logger. This phase proves
the board *generates* the right one — a different question, and the one that
matters when something actually goes wrong downhole.

| Step | Test | Notes |
|---|---|---|
| 4.1 | Test E | Provoke each cause at its real source; check the wire |
| 4.2 | Test E | **Priority ladder** — provoke two at once, confirm only the higher appears |
| 4.3 | Test E | With `SIM BAR` armed, provoke an ADC stall → wire shows `ADC_STALL`, not the synthetic value |
| 4.4 | Test E | Encoder edges: `OVER_RANGE` above 10100 dbar, `UNDER_RANGE` below −50 dbar, and the clamp band 10000–10100 → exactly `10000` |
| 4.5 | Test 6 | NVM saves under the fence, scope-triggered — no torn packets |
| 4.6 | Test 7 | Settings persistence across a power cycle |

Step 4.3 is the safety check: synthetic data must never be able to mask a real
rig fault. Needs no pressure source — grounding or floating the probe inputs
and collapsing VDDEXT is enough.

**Gate:** every cause produces its own code; the ladder holds; no torn packets.

---

## Phase 5 — 24-hour endurance, board alone (adapter only, 24 h)

**Test D** on the Bench target. 2,160,000 packets, ~8.6 MB at the tap.

Start the capture, arm, and leave it. Do not touch the console during the run —
the 5-minute auto-relock is a backstop, not a plan.

**Gate — all of:**
- `RESULT: PASS`
- `coverage : 100.0000%`
- `resets : none`
- All 362 ramps within tolerance
- `checksum err : 0` over the whole run
- No silence beyond 75 ms
- `aborts=0 skips=0` in `STATUS` afterwards

**Record:** packets received of 2,160,000; worst gap; worst ramp error.

> **This is the campaign's main gate.** The replica logger does not go on the
> wire until this passes. A board that cannot stream cleanly to a passive
> adapter for a day will not stream cleanly to a logger, and debugging it with
> the logger attached means two unknowns instead of one.

---

## Phase 6 — First stream to the replica (supervised, ~2 h)

Rig per **Test 15b**. Flash the **Bench-autostart** target.

| Step | What |
|---|---|
| 6.1 | Logger RX joins P1.0 in parallel; tap stays |
| 6.2 | **Disconnect the adapter's TX wire** — keep RX + ground only |
| 6.3 | All three grounds common (bench supply, logger, host PC) |
| 6.4 | Board powered **first**; adapter plugged in after |
| 6.5 | Start `soak_capture.py` (no `--arm` — autostart handles it), **then** press reset |
| 6.6 | Watch the first two minutes live |
| 6.7 | Run ~1 h, stop, and compare three ways |

**What you should see in the first two minutes** (`RATE 1000`):

| From reset | Wire |
|---|---|
| ~0 ms | `NO_READING` |
| ~1 s – 31 s | **start beacon: full scale, 1000.0 bar** |
| 31 s – 66 s | status codes `FF01`…`FF07` |
| 66 s – 126 s | 0 bar |
| from ~126 s | slow climb, 0.1 bar/s |

The beacon railing the logger's chart at full scale for 30 s is your
confirmation that the logger is receiving and decoding at all — before you
commit an hour to it.

**The three-way comparison** is the point of this phase:

1. Reference stream (what the firmware intended)
2. Tap capture (what the board actually put on the wire)
3. Logger's own dump (what the logger recorded)

1 vs 2 is the board. 2 vs 3 is the logger. Keeping the tap is what lets you
tell those apart — without it, a disagreement is unattributable.

**Gate:** tap passes `soak_verify.py`, and the logger's dump matches the tap.

**On failure:** if 1 vs 2 passes but 2 vs 3 fails, the board is fine and the
finding belongs to the logger designer. Record it against the
`link_protocol.md` questionnaire rather than changing firmware.

---

## Phase 7 — 24-hour stream to the replica (24 h)

Repeat Phase 5's run with the logger attached. Same gates, plus:

- [ ] Logger's dump compared against the tap over the full 24 h
- [ ] Logger's staleness handling observed (pull power briefly, mid-run, near
      the end — confirm it records a no-data marker rather than repeating the
      last value)

**Record against `link_protocol.md`'s questionnaire**, which is the deployment
gate list. This run produces real evidence for:

| Gate | What this run answers |
|---|---|
| Q7 | Logger's actual record rate vs. the ≥2× rule, and whether it applies to average rate or maximum gap |
| Q9 | How it recovers from a checksum-invalid packet |
| Q10 | Whether status codes are excluded from averaging |
| Q11 / Q18 / Q19 | Staleness timeout, what gets recorded when stale, partial-packet tolerance |
| Q16 / Q17 | Idle-state behaviour and who holds the line during reset |

**Gate:** 24 h clean on both records, and the questionnaire answered and dated.

---

## Phase 8 — Re-introduce the processing path (optional, after calibration)

Everything above deliberately bypasses the calibration maths. Once the
transducer track has produced a stored calibration, `SIM COUNTS` puts it back
in the loop: the profile is back-solved into synthetic ADC counts, and the real
`calibration_apply()` → `link_encode_bar()` path runs on the way out.

**Scored with a tolerance, not value-exact.** One 12-bit-scaled count is
~2.4 dbar against the 0.1 dbar wire LSB, so the profile's 1 dbar steps
necessarily quantise into a staircase. This checks the maths is sane and
monotonic, not that it reproduces the reference.

---

## Phase 9 — Deployment gates

Not test steps — the remaining blockers before anything goes downhole.

- [ ] Logger questionnaire answered and dated in `link_protocol.md` (**Q11
      staleness above all** — a dead board is silence, and a logger that
      repeats the last value makes a tool that died at hour 2 look identical to
      70 flat hours)
- [ ] Harness pull-up on P1.0 fitted and specified (value + rail) — currently
      required by four documents and specified by none
- [ ] Production target built with `LINK_CONSOLE_EN=0` **and** `APP_ENABLE_SIM=0`
- [ ] Pre-deployment check: send `CONSOLE UNLOCK` to the production build →
      silence confirms the console is compiled out
- [ ] IROM1 shrink to `0xF000` (project-file change — yours)
- [ ] TEMP diagnostics stripped, if that is still wanted
- [ ] Re-calibrate on a rig — flashing wipes settings and calibration by design

---

## Time budget

| Phase | Duration | Attended? |
|---|---|---|
| 0 — desk gates | minutes (plus compile fixes) | yes |
| 1 — bring-up | ~1 h | yes, scope |
| 2 — measurement chain | ~1.5 h | mostly |
| 3 — resolution sweep | 5 h 43 m | no |
| 4 — fault paths | ~1 h | yes |
| 5 — 24 h alone | 24 h | no |
| 6 — first replica stream | ~2 h | first 2 min, then no |
| 7 — 24 h with replica | 24 h | no |

Roughly **four days** with two overnight runs, assuming Phase 0 does not turn
up much.

A sensible calendar:

- **Day 1** — Phases 0, 1, 2. Start Phase 3 overnight.
- **Day 2** — Read Phase 3. Phase 4. Start Phase 5 overnight.
- **Day 3** — Read Phase 5. **Gate decision.** Phase 6. Start Phase 7 overnight.
- **Day 4** — Read Phase 7. Answer the questionnaire. Phase 9 gates.

---

## Standing rules for every run

- **J-Link detached.** WDT1 is disabled in debug mode, so watchdog regressions
  are invisible under the debugger — the exact class of failure a soak exists
  to find.
- **External pull-up on P1.0 fitted.** It is high-Z during reset; without the
  pull-up a reset reads as floating noise rather than clean idle, corrupting
  the evidence.
- **Never change `RATE` mid-run.** It rescales the profile and invalidates the
  reference stream you are scoring against.
- **No settings commands on a timer.** `RATE`, `THRESH`, `RANGE`, `PROBE` and
  `CAL STORE` each write NVM (30,000 erase/program cycles).
- **Console stays LOCKED during soaks.** Unlocking suspends the packet stream.
- **`SIM OFF` after every bench soak.** Sim state is RAM-only, but leaving it
  armed makes the next test lie.
- **Start the capture, then reset the board.** Alignment is unambiguous from
  index 0, and you get the whole run.

---

## Results summary

| Phase | Gate | Pass/Fail | Date | Notes |
|---|---|---|---|---|
| 0 | Desk gates + 3 builds clean | | | |
| 1 | Wire timing in spec (Test 2 ★) | | | |
| 2 | Ladder: 20/20 ramps in tolerance | | | |
| 3 | 10,001 / 10,001 codes | | | |
| 4 | Fault causes + priority ladder | | | |
| 5 | **24 h clean, board alone** | | | |
| 6 | Three-way comparison agrees | | | |
| 7 | 24 h clean with replica | | | |
| 8 | `SIM COUNTS` within quantisation | | | |
| 9 | Deployment gates closed | | | |

Phase 5 is the gate that decides whether the replica goes on the wire.
