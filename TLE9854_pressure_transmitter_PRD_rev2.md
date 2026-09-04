# TLE9854QXW Pressure Transmitter — PRD · Rev 2

_Rev 2 — 14 Jul 2026. Supersedes Rev 1 (analog output). Rewritten for the
digital-link rearchitecture after the full requirements review. Companion
docs: `link_protocol.md` (wire spec + logger questionnaire),
`uart_command_reference.md` (bench surface), `verification_guide.md`._

## 1. Summary

A downhole pressure transmitter built on the **TLE9854QXW** (Arm Cortex-M0,
bare-metal). It reads two conditioned strain-gauge pressure probes (amplified
~0.16 mV/bar bridges), applies a multi-point linear calibration **on-board**,
and continuously transmits the result as a **one-way digital UART packet
stream** to a smart battery/logger — the **only** interface available
downhole. A **bench-only debug console** shares the same UART under strict
mutual exclusion; production firmware has no console at all.

- **The logger stays dumb**: it performs no calibration, compensation, or
  unit conversion — it stores the received 16-bit value verbatim (or averages
  valid pressure codes only). All intelligence, including the future
  temperature-compensation LUT, lives on the board and never changes the wire
  contract.
- Accuracy target: **±1–2 bar after multi-point calibration at reference
  temperature — CONDITIONAL on bench verification of front-end gain and ≥1
  LSB noise (verification guide Test 9)**. Wire resolution 0.1 bar; ADC path
  ~0.24 bar/step (12-bit-scaled, effective bits pending the same evidence).
- Removed from scope vs Rev 1: the analog 0–5 V output and its NAMUR fault
  bands; the runtime `RANGE` output window (the wire scale is fixed
  absolute); the v2 synced-sleep mode and its seams (**dropped entirely** —
  continuous streaming is the operating model and the dead-line staleness
  rule depends on it).

## 2. Scope

**In:** 2-probe acquisition + averaging (12-bit-scaled oversampling);
multi-point linear calibration over the bench console; continuous packet
stream per `link_protocol.md`; distinct fault-cause codes; settable sample
rate (NVM); status LED; TX-purity guarantees (locked console + production
compile-out); host-side passive monitor + bench tool; power readout
(placeholder pending measurement).

**Out:** synced sleep (dropped); temperature-compensated calibration LUT
(future, separable — architecture must not block it: applied before encoding,
invisible to the logger); any logger-side processing.

## 3. Hardware / pin map

| Function | Pin(s) | Notes |
|---|---|---|
| MCU | TLE9854QXW | Cortex-M0, Keil MDK + TLE985x DFP |
| Probe ADC inputs | **AN7 (P2.7), AN3 (P2.3)** | ADC1, 10-bit native; amplified bridge signals |
| Excitation | VDDEXT → BJT mirror → both bridges | firmware-enabled, always on; supervised per refresh |
| **Downhole link + console** | **TX P1.0 / RX P1.1 (UART2), 9600 8N1** | one shared line; P1.0 high-Z during reset → **harness pull-up required** (gate Q17) |
| Status LED | **P0.4** | GPIO push-pull |
| Unused by design | P0.1 (old PWM), **P0.2/UART1 (SWD debug strap — never drive)** | |

## 4. Functional requirements

### 4.1 Acquisition
- Sample both probes on ADC1; 16× oversample, **sum/4** → 12-bit-scaled
  counts (0–4092; `ADC_COUNTS_MAX` is a max code, never a conversion
  denominator — that is 4096).
- Cross-check: probes differing by more than `THRESH` (default 80 counts
  ≈ 2 % FS, NVM) raise the DISAGREE fault with ~thresh/8 hysteresis.
- Supervision per refresh: ADC stall and VDDEXT instability are **distinct
  fault causes** (distinct wire codes).
- Bounded waits everywhere — never spin on hardware status; a stalled ADC
  costs ≤ ~34 ms per refresh and is fenced (see 4.3).

### 4.2 Calibration — multi-point linear (on-board, seam for the future LUT)
- Bench-console capture flow unchanged from Rev 1 (`CAL ARM` → `CAL <bar>` ×N
  → `CAL STORE`); least-squares fit; NVM-persisted with magic versioning.
- Sensor absolute, zeroed at ambient. **Recalibration is required after any
  front-end gain adjustment and after the Rev-2 firmware flash** (count scale
  changed; old cal intentionally rejected).
- Uncalibrated boards transmit the `UNCAL` status code — never raw counts
  presented as pressure.

### 4.3 Output — the downhole interface (replaces Rev 1 §4.3 entirely)
- Wire contract: **`link_protocol.md` is normative** (9600 8N1; sync `0x7F`;
  >2 ms gap; MSB/LSB/checksum; <10 ms packet; >20 ms idle; 110 ms period;
  payload sacred — 0x7F legal in payload/checksum; big-endian; additive
  checksum as specified by the logger designer).
- Encoding: 0–10000 = deci-bar absolute; `0xFF01–0xFF07` status codes with
  fixed priority; float-domain clamping and explicit NaN handling.
- **Boot fail-safe:** the stream is alive within milliseconds of power-on
  carrying `NO_READING` until the first sample — the wire never shows a fake
  pressure. A dead board is *silence*, which the **logger must convert to a
  distinct no-data record via a staleness timeout (recommended 500 ms) —
  DEPLOYMENT BLOCKER, questionnaire Q11**.
- **Packet atomicity:** once a sync byte starts, the packet completes.
  Anything that can stall the CPU or mask IRQs for milliseconds (NVM flash
  ops, stalled-ADC refresh, RAW/SCAN bursts) must first acquire the
  **fail-closed link fence** (wire-idle hold; on failure the operation is
  skipped/deferred and reported — never run over a live packet). Aborts exist
  only as a last-resort net for unpredicted stalls, with conservative
  recovery (full idle before the next sync; worst wire artifact = sync +
  silence, never a partial data block).
- Rate/availability: ~9.1 pkt/s nominal; worst valid-packet gap ≈ 145 ms;
  ≈ 24 pkt/s sustained during an ADC fault; supports logger recording
  ≤ ~4–4.5 Hz (gate Q7: rate vs max-gap semantics — now load-bearing).

### 4.4 Bench console + TX purity (replaces Rev 1 §4.4)
- **Production firmware emits packets and nothing else** — console compiled
  out (`LINK_CONSOLE_EN=0`).
- Debug firmware **boots locked** (zero TX text); exact `CONSOLE UNLOCK`
  suspends the stream (mutual exclusion — packets and text never share the
  wire); `CONSOLE LOCK` / 5-min inactivity / power cycle re-lock and resume.
- Command set per `uart_command_reference.md`; `LINKTEST` (RAM-only,
  5-min auto-expiry) replaces the old OUTPUT override; interrupt-driven
  ring-buffer I/O, strictly non-blocking, WDT-safe.

### 4.5 Settable rate
- `RATE` sets the sample/refresh cadence (100–5000 ms, NVM). The packet
  stream repeats the latest value at its own fixed cadence regardless.

### 4.6 Power readout
- Debug convenience only. Characterized table pending bench measurement of
  the continuous-TX profile (verification guide Test 13); placeholder until
  then.

### 4.7 Status LED (P0.4)
- Unchanged: heartbeat / cal-armed / capturing / stored / fault patterns,
  centrally arbitrated in `main.c`.

## 5. Architecture

- Bare-metal cooperative super-loop, 1 ms SysTick, WDT1 serviced only via
  `scheduler_service()`; no blocking waits; long operations fenced.
- Module split: `link_frame` (pure protocol core — **compiles under gcc and
  is host-tested**, the primary pre-hardware verification), `link_tx` (UART2
  shim, TX-owner arbiter, fence), `acquisition`, `calibration`, `fault`
  (per-cause), `nvm_config`, `uart_cmd` (debug builds), `status_led`,
  `scheduler`, `main`.
- The refresh pipeline is latched (scheduler flag is consume-once) and
  fence-gated with a 3-strike deferral escape.
- Temperature-LUT seam: calibration owns counts→bar; a future
  `(counts, temp)` surface replaces the line fit without touching the wire
  contract or the logger.

## 6. Constraints

- **WDT1** as Rev 1 (never a second init/trigger; ~300 ms budget; 5 resets =
  Sleep latch). **VDDEXT** firmware-enabled, bounded settle wait, supervised.
- **NVM data flash**: BootROM `user_nvm_*`, IRQ-masked ~5–10 ms stalls —
  fence-gated, fail-closed, health-gated (`nvm_flash_is_healthy`).
- **RTE/ vendor code untouched** (this rearchitecture required zero ISR
  config edits — UART2's existing callback wiring is reused).
- Units: bar-native; PSI only at the console boundary.
- IEEE-754 single float, no fast-math — host-test vectors transfer to
  ARMCLANG.

## 7. External requirements (on the logger — NEW section)

Normative list + questionnaire: `link_protocol.md` §5–§8. Highlights:
1. **Staleness timeout → distinct no-data record (DEPLOYMENT BLOCKER).**
2. Fault-page codes stored verbatim; excluded from any pressure averaging.
3. Gap-based framing / per-packet auto-baud / byte-order confirmations.
4. Harness provides the line's idle-high pull during transmitter reset.
5. Logger record rate vs the ~9.1 pkt/s stream (2× rule semantics).

## 8. Verification strategy

- **Host-executable (pre-hardware, every commit):** gcc protocol tests with a
  virtual-wire simulation (timing invariants, stall/fence/abort scenarios,
  encode boundary vectors incl. NaN/±Inf, golden vectors) + Python decoder
  tests cross-checked against the firmware simulation's byte-exact capture.
- **On hardware (customer, `verification_guide.md`):** scope-verified packet
  timing (the ★ test), TI-offset measurement, packets-only boot, mutual
  exclusion, LINKTEST 0x7F pass-through vectors, NVM-under-fence, fault codes
  on the wire, RAW noise capture (ENOB evidence for the 12-bit claim),
  watchdog soaks, calibration against a reference gauge.
- **Keil build** is the only firmware build gate (host syntax sweeps are a
  net, not a substitute).

## 9. Acceptance criteria (v2)

- Flashes and runs standalone (J-Link detached); no watchdog resets across
  both soak tests.
- **From power-on the wire carries valid packets and nothing else** (all
  builds; scope-verified): `NO_READING` → `UNCAL`/pressure.
- Scope timing within limits: packet < 9.5 ms, gap 2.9–5.0 ms, idle ≥ 22 ms,
  period 110 ms, baud within ±2 %.
- 5-minute soak: zero checksum errors at the host monitor, ≈ 9.1 pkt/s.
- Every induced fault appears as its distinct code on the wire; recovery
  returns the live value.
- LINKTEST vectors byte-exact, including `0x7F` in LSB and checksum
  positions; auto-expiry works.
- NVM saves produce zero malformed/truncated frames (scope-triggered);
  failed saves are reported and leave state unchanged.
- Calibration: wire deci-bar matches the reference gauge within the accuracy
  target; persists across power cycles; `CAL CLEAR` → `UNCAL` on the wire.
- Console: locked at boot, mutual exclusion verified, auto-relock works;
  production build answers nothing.
- Deployment additionally gated on: logger questionnaire answers (staleness
  above all) + harness pull-up.

## 10. Open / TODO

- [ ] Logger-designer questionnaire (`link_protocol.md` §7) — answers
      recorded and dated. **Q11 staleness = deployment blocker.**
- [ ] Harness idle-high pull-up on the P1.0 line (Q17).
- [ ] Bench: RAW noise capture (ENOB verdict for the 12-bit claim) +
      front-end counts/bar measurement.
- [ ] Bench: TI-offset scope measurement (timing hypothesis).
- [ ] Power: measure continuous-TX consumption; replace the 40 mW
      placeholder.
- [ ] Keil: production target with `-DLINK_CONSOLE_EN=0`; IROM1 shrink to
      0xF000 (pre-existing item).
- [ ] Future (separable): temperature source decision (die temp vs sensor
      temp vs external) → temperature-compensation LUT in calibration.
