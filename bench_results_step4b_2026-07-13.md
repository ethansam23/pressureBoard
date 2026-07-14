# Bench Verification Record — step 4b build

_Fill this in during the bench session. Template generated 2026-07-13; keep
`verification_guide.md` open beside it — that file is the procedure, this one
is the evidence record. Commit this file once filled (it is the step-5 input)._

## Firmware under test

| Item | Value |
|---|---|
| Branch / commit | `claude/rearchitecture` @ `0593376` ("keil: project update for link modules (step 4b)") |
| Build | ARMCLANG V6.24, -O0, target `Target_1`, **0 errors / 0 warnings** |
| Program size | Code=21140 RO-data=3316 RW-data=20 ZI-data=2144 |
| Image | `Objects\dh_pressureboard_attempt1.axf` (built 2026-07-13 on the Windows machine) |
| Known build notes | `-std=c99` in force → the C11-gated `_Static_assert`s in `acquisition.c:68-72` compile out (reported, not changed). Console compiled IN (`LINK_CONSOLE_EN` default) — this is the debug/bench build; production compile-out target is a separate open item. |
| Host gates | `host_ui` unittest 16/16 green on this machine. `make -C host_tests` NOT run here (no gcc) — no `link_frame.c` edits were made, so the committed golden stream stands; re-run on the dev machine before merge. |

## Pre-flight (before Test 1)

- [ ] **µVision reload**: two UV4 instances have been open since early June.
      If either has this project loaded, let it RELOAD from disk (or close &
      reopen the project) before building/flashing from the IDE — and do not
      let a stale in-memory copy re-save over commit `0593376`.
- [ ] Flash via J-Link from the IDE, then **detach J-Link and power-cycle**
      for every boot/watchdog item (WDT1 is disabled under debug).
- [ ] Terminal / host monitor at **9600 8N1** (not 115200).
- [ ] Scope or LA on **P1.0** (mandatory).
- [ ] Expect first boot to wipe settings + calibration (both NVM magics
      bumped): defaults + `0xFF02` UNCAL until Tests 10–11.
- [ ] Monitor: `cd host_ui && python pressure_monitor.py` — passive mode,
      do NOT enter Bench Mode until Test 3. (Rate-tile fix applied
      2026-07-13 — Test 4's ≈25.0/s expectation is now valid; before the fix
      the tile showed chunk cadence ~10/s.)
- [ ] Terminal encoding UTF-8: the banner / suspended-notice / goodbye lines
      contain em dashes (3-byte UTF-8); a Latin-1 terminal shows mojibake
      there — cosmetic, not a failure.

---

## Test 1 — Packets-only boot (fail-safe) — ALIVE GATE

| Check | Result | Notes |
|---|---|---|
| Zero ASCII at boot (binary-only stream) | | |
| First sync byte within a few ms of power-on (scope) | ms: | |
| Monitor decodes `0xFF01` NO_READING → `0xFF02` UNCAL | | fresh flash ⇒ UNCAL |
| LED heartbeat 100 ms on / 2900 ms off, no boot loop | | |
| 5× power cycle, stream restarts cleanly every time | /5 | |

## Test 2 — Packet timing on the wire ★ — ALIVE GATE

| Measurement | Limit | Nominal | Worst observed |
|---|---|---|---|
| Total packet (sync start bit → CHK stop-bit end) | < 9.5 ms | | |
| Gap (sync stop-bit end → first data start bit) | 2.9 – 5.0 ms | | |
| Spacing between the 3 data bytes | no gap > 1 bit | | |
| Idle (CHK end → next activity) | ≥ 21.8 ms (audit-corrected; logger floor >20) | | |
| Period (sync-start → sync-start) | 40 ms ± 1 ms | | |
| Bit width | 104.2 µs ± 2 % | | |
| Idle-high voltage | per logger divider spec | | |

- TI-offset check (LINKTEST + scope vs STATUS cadence) — TI leads stop-bit
  end by ~1 bit-time? : ______
- NRST held: P1.0 high-Z confirmed? (float noted if no pull-up fitted) : ______

## Test 3 — Console mutual exclusion — ALIVE GATE

| Check | Result | Notes |
|---|---|---|
| `STATUS` while locked → nothing | | |
| `CONSOLE UNLOCK` → in-flight packet completes atomically, then silence | | scope |
| Banner `== Pressure Transmitter v2 (digital link) ==` + suspended notice + `Boot RST 0x…` `[POR]`/`[PIN]` (not `[WDT1]`) | | |
| `STATUS` 5 lines incl. `mode=CONSOLE (stream suspended)`, `pkts=` ≈ 25×s (in packet mode; freezes while unlocked) | | |
| `CONSOLE LOCK` → goodbye, ≥21.8 ms quiet, ONE packet, then 40 ms cadence | | scope |
| 5+ min idle → auto-relock, stream resumes | | |
| Monitor Bench Mode does the same; passive mode logs blocked commands | | |

**ALIVE verdict (Tests 1–3): PASS / FAIL** : ______

---

## Test 4 — Stream soak + host decoder
Chk errors after 5 min: ____  · Packets/s: ____ · STALE within ~0.5 s of power pull: ____

## Test 5 — LINKTEST vectors (payload sacred)

| Command | Expected wire bytes | Scope byte-exact? | Monitor decode |
|---|---|---|---|
| `LINKTEST 10000` | `7F … 27 10 C8` | | 1000.0 bar |
| `LINKTEST 127` | `7F … 00 7F 80` | | 12.7 bar |
| `LINKTEST 383` | `7F … 01 7F 7F` | | 38.3 bar |
| `LINKTEST 65281` | `7F … FF 01 FF` | | NO_READING |

`STATUS` shows `TEST(!)`: ____ · `LINKTEST OFF` resumes live: ____ · 5-min auto-expiry: ____

## Test 6 — NVM saves under the fence
Scope trigger (frame > 9.5 ms or short frame) count over all save commands: ____ (must be 0)
First packet after `CONSOLE LOCK` complete/valid: ____ · `(saved)` echo + `rc=0` each time: ____

## Test 7 — Settings persistence
`RATE 500` cadence ~2/s + persists: ____ · `RATE 50` → `ERR: rate 100-5000`: ____
`THRESH 100` persists: ____ · `THRESH 0`/`5000` → `ERR: thresh 1-4092`: ____
STATUS counts 12-bit-scaled (~4× RAW): ____ · cleanup done (`RATE 1000`, `THRESH 80`): ____

## Test 8 — Fault codes on the wire
DISAGREE `0xFF03` + double-blink + auto-clear: ____
VDDEXT `0xFF05` (if inducible) + recovery: ____
ADC_STALL `0xFF04` (if inducible): rate ≈24/s, zero malformed, max sync-to-sync ≤ ~78 ms: ____
Priority (two faults → higher code wins): ____

## Test 9 — RAW noise capture (ENOB evidence)
Spread A: ____ B: ____ (≥ 1 native LSB required) · periodic pattern: ____
12-bit verdict: YES / NO / PARTIAL · counts/bar measured: ____

## Test 10 — Magic-bump reset + re-entry
First boot defaults (`Rate: 1000ms  Thresh: 80  NVM: ok`, `Cal: NONE`, wire UNCAL): ____
Re-entered settings persist: ____

## Test 11 — Calibration + wire verification
slope=______ offset=______ (slope ≈ ¼ of old-firmware value for same sensor)
Gauge ______ bar vs decoded ______ bar · power-cycle persistence: ____
`CAL CLEAR` → UNCAL on wire: ____ · error paths (unarmed / <2 pts / 9th pt): ____

## Test 12 — Watchdog soak (standalone, J-Link detached)
Packet-mode 10 min: resets ____ (must be 0) · aborts=____ skips=____
Console-mode `RATE 100` 10 min: resets ____ · unlock banner shows `[WDT1]`? ____ (must be no)

## Test 13 — Power
`POWER` placeholder prints: ____ · measured ____ mA @ ____ V in packet mode

## Test 14 — LED patterns
Heartbeat ____ · cal-armed 1 Hz ____ · capturing 5 Hz ____ · stored solid 2 s ____ · fault double-blink ____

---

## Results summary → PRD Rev 2 §9 mapping

| PRD §9 criterion | Evidence (test #) | Pass/Fail |
|---|---|---|
| Runs standalone, no WDT resets in both soaks | 1, 12 | |
| Power-on: valid packets and nothing else (`NO_READING` → `UNCAL`/pressure) | 1, 2 | |
| Scope timing: packet <9.5 ms, gap 2.9–5.0 ms, idle ≥22 ms, period 40 ms, baud ±2 % | 2 | |
| 5-min soak: zero checksum errors, ≈25 pkt/s | 4 | |
| Every induced fault = distinct wire code; recovery returns live value | 8 | |
| LINKTEST byte-exact incl. 0x7F in LSB/CHK; auto-expiry | 5 | |
| NVM saves: zero malformed frames; failed saves reported, state unchanged | 6 | |
| Calibration: wire matches gauge, persists, `CAL CLEAR` → UNCAL | 11 | |
| Console: locked at boot, mutual exclusion, auto-relock | 3 | |
| (production build answers nothing) | — | OPEN: production target not yet created |
| Deployment gates: logger questionnaire (Q11) + harness pull-up (Q17) | — | OPEN: external |
