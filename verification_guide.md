# Pressure Transmitter v2 (digital link) — Verification Guide

On-hardware test procedure for the digital-link firmware. **Run standalone,
J-Link detached, for every boot/watchdog item** — WDT1 is disabled in debug
mode, so watchdog regressions are invisible under the debugger.

Equipment:
- TLE9854QXW board, powered (5 V)
- USB-UART adapter on TX P1.0 / RX P1.1 — **9600 8N1** (this line IS the
  logger line; the console shares it under mutual exclusion). **The adapter
  must signal at 5 V** — see the pin table below
- **Oscilloscope or logic analyzer on P1.0** — mandatory, not optional: it is
  the only instrument that can verify the wire timing the logger depends on
- Host monitor (`host_ui/pressure_monitor.py`) — passive packet decoder +
  bench console
- Reference pressure source + gauge (calibration tests)

### Pins and levels

| Signal | Pin | Package | Notes |
|---|---|---|---|
| **Link TX** (board → logger / adapter RX) | **P1.0** | 31 | UART2 TXD, ALT3, push-pull |
| **Console RX** (adapter TX → board) | **P1.1** | 32 | UART2 RXD, input + firmware pull-up |
| Status LED | P0.4 | 28 | |
| Probe A / Probe B | P2.7 (AN7) / P2.3 (AN3) | 37 / 38 | ADC1 ch 12 / ch 9 |
| Sensor excitation | VDDEXT | 45 | 5.0 V, 40 mA |

Port pins run from **VDDP = 5.0 V**, and the two numbers that matter are:

- P1.0 drives **V_OH ≥ 4.6 V** push-pull (datasheet Table 39). Into an adapter
  RX that is not 5 V-tolerant, **the board can damage the adapter.** This is
  the leg to get right.
- P1.1 needs **V_IH ≥ 0.7 × VDDP = 3.5 V**. A 3.3 V adapter TX sits in the
  undefined band — it is not a guaranteed logic high, and it fights the
  firmware's 5 V pull-up. Not destructive (well inside the VDDP + 0.3 V
  absolute max), but console commands may be flaky or dead.

Use a 5 V adapter, or a level shifter. Both legs are fine at 9600.

Record **nominal and worst observed** values for every timing measurement.

> **First boot after flashing this firmware:** both NVM magics were bumped —
> settings AND calibration are intentionally reset. Expect defaults + UNCAL
> until Tests 10–11 re-establish them.

---

## Test 1 — Packets-only boot (fail-safe)

Power-cycle, J-Link detached, terminal AND scope attached.

- [ ] **Zero ASCII at boot** — no banner, no diagnostics; the terminal shows
      only binary garbage (the packet stream at 9600). A board that "prints
      nothing" is CORRECT.
- [ ] Scope: first sync byte within a few ms of power-on
- [ ] Host monitor (passive): decodes `0xFF01` NO_READING immediately, then
      `0xFF02` UNCAL (fresh flash) or live pressure after ~1 refresh
- [ ] LED heartbeat (100 ms on / 2900 ms off); no boot loop (a repeating
      restart pattern that goes dead after ~5 resets = WDT1/Sleep latch)
- [ ] Power cycle 5×: stream restarts cleanly every time

## Test 2 — Packet timing on the wire (scope/LA) ★ the core test

Capture several packets; measure:

| Measurement | Limit | Nominal | Worst observed |
|---|---|---|---|
| Sync start bit → checksum stop-bit end (total packet) | **< 9.5 ms** | ~7.6–8.3 ms | _____ |
| Sync stop-bit end → first data start bit (gap) | **2.9 – 5.0 ms** | ~3.4–4.1 ms | _____ |
| Spacing between the 3 data bytes | back-to-back (no gap > 1 bit) | 0 | _____ |
| Checksum end → next line activity (idle) | **≥ 21.8 ms** | ~31 ms | _____ |
| Sync-start → sync-start (period) | 40 ms ± 1 ms | 40 ms | _____ |
| Bit width (baud accuracy) | 104.2 µs ± 2 % | 104.2 µs | _____ |
| Idle-high voltage | per logger divider spec | _____ | _____ |

> Nominal-column note (2026-07-13 pre-bench audit): on any single packet,
> total = gap + 4.17 ms (four bytes at 9600 8N1, data back-to-back); the
> ranges above are the SysTick tick-phase span seen in the host simulation.
> Idle-floor note: the firmware enforces 23 ms in TI-tick arithmetic; tick
> quantization minus the 1-bit TI-lead hypothesis puts the guaranteed wire
> idle at ≥ 21.8 ms. A measured ~21.9 ms (console-LOCK resume path is the
> only case that can approach it) is in-spec — the logger floor is > 20 ms —
> not a failure. ≥ 22.0 ms is guaranteed only if the TI-offset check below
> shows TI at the stop-bit END.

- [ ] **TI-offset check (resolves the timing hypothesis):** with `LINKTEST`
      toggling a known code, compare a scope-measured byte end (stop-bit
      edge) against the firmware's reported timing (STATUS counters cadence).
      Record whether TI leads the stop-bit end by ~1 bit-time: _____
- [ ] **Reset line state:** hold the MCU in reset (NRST) — confirm P1.0 goes
      high-Z (line level then defined only by the external pull-up; if none
      is fitted yet, note the float — harness pull-up is a deployment
      requirement, `link_protocol.md` Q17)

## Test 3 — Console mutual exclusion

1. Terminal at 9600. Type `STATUS` + Enter → **nothing happens** (locked).
2. Type `CONSOLE UNLOCK` + Enter.

- [ ] Scope: the in-flight packet **completes atomically** (never truncated),
      then the stream stops
- [ ] Banner prints: `== Pressure Transmitter v2 (digital link) ==`,
      stream-suspended notice, `Boot RST 0x…` with `[POR]`/`[PIN]` (NOT
      `[WDT1]`)
- [ ] Echo/backspace now work; `STATUS` prints 5 lines incl.
      `mode=CONSOLE (stream suspended)` and `pkts=` (a number ~25×seconds
      spent in packet mode — the counter freezes while unlocked, so this
      ≈ seconds-since-boot only here at the first unlock)
- [ ] `CONSOLE LOCK` → goodbye line, then scope shows ≥ 21.8 ms quiet (see
      Test 2 idle-floor note), then a SINGLE packet (no burst), then normal
      40 ms cadence
- [ ] Unlock again, wait 5+ min without typing → firmware auto-relocks and
      the stream resumes on its own
- [ ] Host monitor: "Enter Bench Mode" does all of the above from the GUI;
      passive mode provably sends nothing (log shows blocked commands)

## Test 4 — Stream soak + host decoder

Passive monitor connected, 5+ minutes:

- [ ] `Chk errors` stays **0**; `Packets` climbs ~25/s; `Rate` ≈ 25.0/s
- [ ] `Link` tile shows `live` throughout (never STALE while powered)
- [ ] Pull the board's power mid-soak → STALE indicator within ~0.5 s (this
      is the logger's staleness scenario made visible)

## Test 5 — LINKTEST end-to-end (payload-sacred vectors)

Unlock, then:

| Command | Expected wire bytes after `CONSOLE LOCK` |
|---|---|
| `LINKTEST 10000` | `7F … 27 10 C8` |
| `LINKTEST 127` | `7F … 00 7F 80` — **0x7F transmitted verbatim in the LSB** |
| `LINKTEST 383` | `7F … 01 7F 7F` — **0x7F in LSB and checksum** |
| `LINKTEST 65281` | `7F … FF 01 FF` (decodes as NO_READING) |

- [ ] Each vector byte-exact on the scope; host monitor decodes each
      (127 → 12.7 bar, 383 → 38.3 bar, 65281 → NO_READING)
- [ ] `STATUS` shows `TEST(!)` while forced
- [ ] `LINKTEST OFF` → live values resume
- [ ] Set a code, don't clear it, wait 5+ min → **auto-expires** (wire back
      to live)

## Test 6 — NVM saves under the fence (scope-triggered)

Scope set to trigger on any frame longer than 9.5 ms or shorter than a full
4-byte packet. Unlock, then run each several times:

- `RATE 500` / `RATE 1000` · `THRESH 100` / `THRESH 80` · `PROBE A` /
  `PROBE AVG` · full `CAL STORE` (Test 11) · `CAL CLEAR`

- [ ] **Zero triggers ever**: no truncated packet, no sync-only fragment, no
      partial data block, no frame > 9.5 ms (saves happen while the stream is
      suspended in console mode; the fence is belt-and-suspenders — this test
      also validates it via the resume path)
- [ ] After `CONSOLE LOCK`, first packet is complete and valid; cadence
      clean
- [ ] Every save echoes `(saved)`; `nvm: … rc=0` (TEMP diag)
- [ ] NVM-failure path visible: with flash unhealthy (if reproducible) the
      reply is `(NVM write failed)` / `ERR: NVM write failed` and stored
      values are unchanged after power cycle

## Test 7 — Settings persistence (12-bit scale)

- [ ] `RATE 500` → AUTO cadence visibly ~2/s; power cycle → `Rate: 500ms`
- [ ] `RATE 50` → `ERR: rate 100-5000`, stored value unchanged
- [ ] `THRESH 100` persists across power cycle; `THRESH 0` / `THRESH 5000` →
      `ERR: thresh 1-4092`
- [ ] STATUS probe counts are 12-bit-scaled (0–4092): roughly 4× the RAW
      display values
- [ ] Cleanup: `RATE 1000`, `THRESH 80`

## Test 8 — Fault codes on the wire

For each inducible fault, watch the **decoded stream** (lock the console —
faults transmit only in packet mode):

- [ ] **DISAGREE:** unlock → `AUTO`, note |A−B|, set `THRESH` below it →
      `Faults: DISAGREE` in STATUS; lock → wire carries `0xFF03`; LED
      double-blink. Restore threshold → auto-clears, wire returns to
      pressure/UNCAL
- [ ] **VDDEXT (0xFF05):** if the excitation rail can be disturbed/shorted
      safely, wire shows `0xFF05` while unstable, recovers after (supervision
      re-enables the regulator)
- [ ] **ADC_STALL (0xFF04):** by inspection unless inducible — if inducible:
      wire carries `0xFF04`, stream stays alive at ≈ 24 pkt/s, **zero
      malformed packets on the scope** (the fence postpones, never tears),
      max sync-to-sync gap ≤ ~78 ms: _____
- [ ] Priority: with two faults active simultaneously, STATUS lists both;
      the wire carries the higher one (ADC_STALL > VDDEXT > DISAGREE)

## Test 9 — RAW noise capture (ENOB evidence — the 12-bit claim)

Stable pressure on both probes, unlock:

1. Run `RAW` 30+ times; record `avg/min/max` per channel each time.
2. Compute per-channel: histogram of avg, min-max spread, standard deviation.

- [ ] Sample-to-sample spread ≥ 1 native LSB (the dither the oversampling
      gain requires). Spread A: _____ B: _____
- [ ] No obvious periodic pattern (mains/switching interference) in repeated
      readings
- [ ] Verdict for the record: 12-bit-scaled counts carry real extra
      resolution? YES / NO / PARTIAL — attach data
- [ ] Front-end sanity: RAW `mV` per applied bar across two known pressures
      → measured counts/bar = _____ (documents the amplifier gain)

## Test 10 — Settings re-entry after flash (magic bumps)

Fresh flash of this firmware over the old one:

- [ ] First boot: defaults active (`Rate: 1000ms  Thresh: 80  NVM: ok` —
      two spaces between fields, NVM health field always present), `Cal: NONE`,
      wire shows `0xFF02` UNCAL — old NVM intentionally rejected
- [ ] Re-enter site settings; verify persistence (Test 7)

## Test 11 — Calibration workflow (unchanged flow, new scale)

1. `CONSOLE UNLOCK`, `RATE 100` (fast captures)
2. `CAL ARM` → `Cal ARMED (0 pts)`, LED 1 Hz
3. Ambient: `CAL 14.7 PSI` → `Capturing…` (LED 5 Hz) → `Captured (1 pts)`
4. High reference: `CAL <ref>` → `Captured (2 pts)` (+ optional intermediates,
   ≤ 8)
5. `CAL STORE` → `Cal stored: slope=… offset=…`, LED solid 2 s

- [ ] Slope positive, ≈ (P_hi − P_lo)/(counts_hi − counts_lo) — **counts are
      12-bit-scaled**, so slope is ¼ of the old-firmware value for the same
      sensor. slope=_____ offset=_____
- [ ] `RATE 1000`, `CONSOLE LOCK` → **wire deci-bar matches the reference
      gauge** through the host monitor: gauge _____ bar vs decoded _____ bar
- [ ] Power cycle → `CAL STATUS` identical (pts, slope, offset); wire still
      calibrated
- [ ] `CAL CLEAR` → wire returns to `0xFF02` UNCAL (never raw counts)
- [ ] Error paths: `CAL 500` unarmed → `ERR: CAL ARM first`; `CAL STORE` at
      <2 pts → `ERR: need >=2 pts`; 9th point → `ERR: max 8 pts (STORE or
      ABORT)`

## Test 12 — Watchdog soak (standalone)

J-Link detached, console LOCKED (packet mode), 10+ minutes; then unlocked
with `AUTO` on + `RATE 100`, 10+ minutes:

- [ ] No resets in either mode (host monitor: packet counter never restarts;
      no NO_READING codes mid-run; unlock banner would show `[WDT1]` — check
      it doesn't)
- [ ] Packet-mode soak: aborts=0 skips=0 in STATUS afterwards: _____
- [ ] LED heartbeat steady throughout

## Test 13 — Power readout + baseline measurement

- [ ] `POWER` → `Power: 40 mW (continuous)` (placeholder)
- [ ] **Measure actual supply current** in packet mode (25 pkt/s TX) and note
      for the power-budget update: _____ mA @ _____ V

## Test 14 — LED pattern summary

| State | Pattern | Observed in test |
|-------|---------|------------------|
| Heartbeat | 100 ms on / 2900 ms off | 1: [ ] |
| Cal armed | 1 Hz | 11: [ ] |
| Cal capturing | 5 Hz | 11: [ ] |
| Cal stored | solid 2 s | 11: [ ] |
| Fault | double-blink | 8: [ ] |

---

## Test 15a — Adapter-only rig (do this FIRST — no logger needed)

**The entire T-A…T-D campaign runs here.** The logger is not a prerequisite for
any of it: the tap is the complete board-side proof — does the firmware emit
the right codes, with the right timing, for 24 hours. The logger answers a
different question, covered in 15b.

Firmware built with `-DAPP_ENABLE_SIM=1`. One USB-UART adapter does both jobs,
since TX and RX are separate pins:

```
board TX P1.0 ──────► adapter RX      (the packet stream)
board RX P1.1 ◄────── adapter TX      (console commands)
                GND ── common
```

- [ ] **External pull-up fitted on P1.0.** P1.0 is high-Z while the MCU is in
      reset (`link_protocol.md` §1, gate Q17), so without it a reset presents
      as floating noise rather than clean idle — corrupting exactly the
      evidence these tests exist to collect
- [ ] **J-Link DETACHED.** WDT1 is disabled in debug mode, so watchdog
      regressions are invisible under the debugger
- [ ] Board on a bench supply, not the host PC's USB
- [ ] Adapter signals at **5 V** (see Pins and levels above)
- [ ] Arm and capture in one step:
      `python soak_capture.py --port <COM> --out logs/<run> --arm --sim BAR --phase FULL`
      — it prints the arming steps, verifies the board's RATE, then confirms
      packets are actually flowing before committing to the long run
- [ ] Or arm by hand: `CONSOLE UNLOCK` → `SIM STATUS` (sanity-check `rate=`) →
      `SIM BAR` → **`CONSOLE LOCK`**, close the terminal, then start the
      capture without `--arm`

> **Cleanup:** `SIM OFF` after every soak. Sim state is RAM-only so a power
> cycle also clears it, but leaving it armed makes the next test lie.

## Test 15b — Add the logger (later, and only after 15a passes)

Logger RX joins P1.0 in parallel — its input is high-Z, so tapping is safe.
This answers only whether the **logger** records correctly, and feeds the
`link_protocol.md` gates (Q7 rate, Q11 staleness, Q18/Q19).

```
                    ┌────────────► logger RX          (the real recording)
board TX P1.0 ──────┤
                    └────────────► adapter RX         (tap / independent evidence)
```

**Keep the tap, but remove one wire.** Electrically the parallel tap is a
non-issue: two high-Z receivers add ~5–10 pF plus cable against a push-pull
driver at a 104 µs bit time. The risk is the **TX** leg — with the adapter TX
still on P1.1, anything that writes to that COM port can send `CONSOLE UNLOCK`
and suspend the stream mid-run, leaving the logger recording silence. (The
5-minute auto-relock caps it, but a five-minute hole still ruins the run.)

- [ ] Flash the **autostart** build (`-DAPP_ENABLE_SIM=1 -DAPP_SIM_AUTOSTART=1`)
- [ ] **Disconnect the adapter's TX wire.** Keep RX + ground only. Autostart
      needs no console, so the tap becomes physically incapable of interfering
- [ ] All three grounds tied together (bench supply, logger, host PC)
- [ ] Do **not** leave the adapter plugged into an unpowered board — its TX
      idles high at 5 V and can back-feed through P1.1's ESD diode into VDDP.
      Power the board first; unplug the adapter before powering down
- [ ] Start the capture, **then** press reset. The verifier can align anywhere,
      but starting at index 0 makes alignment unambiguous and captures the
      whole run
- [ ] Compare the logger's own dump against the tap capture — if they disagree,
      that difference is the finding

### The start beacon (autostart builds)

Autostart emits **30 s of full scale (10000 = 1000.0 bar)** before the profile
begins, so the start of a run is unmissable in the logger's dump. It is
boot-only, which makes it the reset detector: exactly one beacon per boot, so a
second one anywhere in a capture means the board rebooted, and `soak_verify.py`
reports it timestamped to the packet.

Boot-to-first-pressure timeline at `RATE 1000`, so a healthy board is not
mistaken for a dead one:

| From reset | Wire shows |
|---|---|
| ~0 ms | `NO_READING` — the stream is alive within milliseconds |
| ~1 s – 31 s | **beacon: `10000`, full scale** |
| 31 s – 66 s | status block `FF01`…`FF07` |
| 66 s – 126 s | held at 0 dbar |
| from ~126 s | the Phase A sweep starts climbing, 1 dbar/s |

**Reset signatures differ by build — know which you flashed:**

| Build | On reset the wire shows |
|---|---|
| Console-armed (`APP_SIM_AUTOSTART=0`) | sim drops out entirely; `UNCAL` forever after, profile never resumes |
| Autostart (`=1`) | a second start beacon, then the profile again from index 0 |

## Test A — Resolution / full range (Phase A, 5 h 43 m)

Proves every deci-bar code the wire can carry actually encodes and transmits.
`SIM PHASE A`, `SIM BAR`, `CONSOLE LOCK`, capture, then:

```
make -C host_tests
./host_tests/test_link_frame --emit-ref ref_a.csv 1000 1
python soak_verify.py --capture logs/<run> --reference ref_a.csv --rate 1000
```

- [ ] `distinct pressure codes seen : 10001` and `coverage : 100.0000%`
- [ ] `checksum err : 0`, `non-frame bytes: 0`
- [ ] `range seen : 0 .. 10000 dbar` — nothing above the cap
- [ ] No divergence; `RESULT: PASS`
- [ ] Codes seen: _____ / 10001   checksum errors: _____

Coverage here is what covers the payload-sacred cases (every code whose LSB or
checksum lands on `0x7F`) without a hand-picked vector list — Test 5 spot-checks
four of them, this sweeps all of them.

## Test B — Ramp timing ladder (Phase B, 1 h)

Twenty fixed-duration ramps at rates from 1 to 1000 dbar/refresh.
`SIM PHASE B`, `SIM BAR`, `CONSOLE LOCK`, capture one full cycle, then verify
against `--emit-ref ref_b.csv 1000 2`.

- [ ] All 20 ramps appear in the RAMP TIMING table
- [ ] Every ramp `ok` — none `OUT OF TOLERANCE`
- [ ] Worst |error|: _____ ms (tolerance is 80 ms or 2%, whichever is larger)
- [ ] `RESULT: PASS`

Durations are measured by counting packets against the 40 ms nominal period,
not by host timestamps — OS serial buffering would otherwise dominate the
error on the 10-second windows.

## Test C — High-resolution stress (Phase B at RATE 100, 1 h)

Same wall-clock ladder with 10× the samples per ramp: the value changes every
2.5 packets instead of every 25.

- [ ] `CONSOLE UNLOCK` → `RATE 100` → `SIM PHASE B` → `SIM BAR` → `CONSOLE LOCK`
      (set `RATE` **before** arming SIM: window durations are converted against
      the current `RATE` on every refresh, so changing it mid-run moves the
      index and makes the reference invalid)
- [ ] Verify with `--emit-ref ref_b100.csv 100 2 … --rate 100`
- [ ] `RESULT: PASS`; `aborts=0 skips=0` in `STATUS` afterwards
- [ ] Restore `RATE 1000` before Test D

## Test D — 24-hour endurance (FULL, 24 h)

The real run: 2,160,000 packets, ~8.6 MB at the tap.
`RATE 1000`, `SIM PHASE FULL`, `SIM BAR`, `CONSOLE LOCK`, capture 24 h.

- [ ] `RESULT: PASS` against `--emit-ref ref_full.csv 1000 0`
- [ ] `coverage : 100.0000%` (Phase A runs first, so all 10,001 codes)
- [ ] **`resets : none`.** On an autostart build this is the start-beacon
      check — exactly one beacon per run, so a second means the board rebooted.
      On a console-armed build the verifier falls back to counting
      `NO_READING` onsets (the profile emits one per cycle by design, 19 in a
      full run) and flags the surplus
- [ ] All 362 ramps within tolerance
- [ ] `checksum err : 0` over the whole run
- [ ] No silence beyond the 75 ms worst legitimate gap
- [ ] `aborts=0 skips=0` in `STATUS` afterwards
- [ ] Packets received: _____ / 2,160,000   resets: _____   worst gap: _____ ms
- [ ] Logger's own recording compared against the tap: _____ (15b only)

> **Do not change `RATE`, `THRESH`, `RANGE`, `PROBE` or run `CAL STORE` during
> the run.** Each writes NVM (30k cycle endurance), and `RATE` would rescale
> the ladder and invalidate the reference.

## Test E — Encoder edges and real fault generation

Tests A–D prove the status codes reach the logger correctly. This proves the
board *generates* the right one. Provoke each at its real source and check the
wire:

| Cause | Expected code | How to provoke |
|---|---|---|
| ADC stalled | `0xFF04` ADC_STALL | starve conversions |
| Excitation down | `0xFF05` VDDEXT | collapse the VDDEXT rail |
| Probe disagreement | `0xFF03` DISAGREE | drive the probes apart past `THRESH` |
| No calibration | `0xFF02` UNCAL | `CAL CLEAR` |
| Boot | `0xFF01` NO_READING | cold power-up |

- [ ] Each cause produces its own code on the wire
- [ ] **Priority ladder**: provoke ADC stall *and* disagreement together →
      only `0xFF04` appears (`ADC_STALL > VDDEXT > DISAGREE > UNCAL`)
- [ ] With `SIM BAR` armed, provoke an ADC stall → the wire shows `0xFF04`,
      **not** the synthetic value (synthetic data must never mask a rig fault)
- [ ] `OVER_RANGE` (`0xFF06`) above 10100 dbar and `UNDER_RANGE` (`0xFF07`)
      below −50 dbar — these are *encoder* edges outside the profile's range,
      so drive them with `LINKTEST`/calibration rather than `SIM`
- [ ] Clamp band: 10000–10100 dbar encodes to exactly `10000`, **not**
      `OVER_RANGE`
- [ ] Mid-run reset: power-cycle during a soak → capture shows the silence,
      and `SIM` is **off** afterwards (state is never persisted)

## Results summary

| Test | Description | Pass/Fail | Notes |
|------|-------------|-----------|-------|
| 1 | Packets-only boot / fail-safe | | |
| 2 | Wire timing (scope) ★ | | |
| 3 | Console mutual exclusion | | |
| 4 | Stream soak + decoder | | |
| 5 | LINKTEST vectors (0x7F pass-through) | | |
| 6 | NVM under fence (no torn frames) | | |
| 7 | Settings persistence | | |
| 8 | Fault codes on the wire | | |
| 9 | RAW noise capture (ENOB) | | |
| 10 | Magic-bump reset + re-entry | | |
| 11 | Calibration + wire verification | | |
| 12 | Watchdog soak | | |
| 13 | Power readout + measurement | | |
| 14 | LED patterns | | |
| 15a | Adapter-only rig (pull-up, J-Link off) | | |
| 15b | Logger added in parallel (TX wire removed) | | |
| A | Resolution / full range — all 10,001 codes | | |
| B | Ramp timing ladder (20 ramps) | | |
| C | High-resolution stress (RATE 100) | | |
| D | 24-hour endurance (2.16M packets) | | |
| E | Encoder edges + real fault generation | | |

**Deployment sign-off additionally requires** the logger-designer
questionnaire answers in `link_protocol.md` (staleness rule Q11 above all)
and the harness idle-high pull-up (Q17).
