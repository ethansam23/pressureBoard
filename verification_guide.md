# Pressure Transmitter v2 (digital link) — Verification Guide

On-hardware test procedure for the digital-link firmware. **Run standalone,
J-Link detached, for every boot/watchdog item** — WDT1 is disabled in debug
mode, so watchdog regressions are invisible under the debugger.

Equipment:
- TLE9854QXW board, powered (5 V)
- USB-UART adapter on TX P1.0 / RX P1.1 — **9600 8N1** (this line IS the
  logger line; the console shares it under mutual exclusion)
- **Oscilloscope or logic analyzer on P1.0** — mandatory, not optional: it is
  the only instrument that can verify the wire timing the logger depends on
- Host monitor (`host_ui/pressure_monitor.py`) — passive packet decoder +
  bench console
- Reference pressure source + gauge (calibration tests)

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

**Deployment sign-off additionally requires** the logger-designer
questionnaire answers in `link_protocol.md` (staleness rule Q11 above all)
and the harness idle-high pull-up (Q17).
