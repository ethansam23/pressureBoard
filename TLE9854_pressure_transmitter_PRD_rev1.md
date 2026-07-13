# TLE9854QXW Pressure Transmitter — PRD · Rev 1

_Rev 1 — 6 Jun 2026. ("v1 / v2" below refer to firmware scope, not the document revision.) Companion to `TLE9854_pressure_transmitter_design_notes.md` (background/decision log)._

## 1. Summary

A downhole pressure transmitter built on the **TLE9854QXW** (Arm Cortex-M0, bare-metal). It reads
two conditioned strain-gauge pressure probes, averages them, applies a linear calibration, and drives
a **continuous 0–5 V analog output** to a smart battery — which is the **only** interface available
downhole. A **bench-only UART** handles calibration, configuration, status, and a power readout.

- Accuracy target: **±1–2 bar** over the configured output window at 10-bit. (The original ±10–15 psi ≈ ±0.7–1.0 bar would need a 12-bit PWM-DAC + oversample-decimate — see §6.) The output window is **runtime-adjustable** (`RANGE` cmd, NVM-persisted) to the tool's real operating range rather than the 1000-bar sensor rating; narrowing it improves resolution proportionally.
- **v1 (this PRD):** continuous, always-valid analog output.
- **v2 (future, seams only):** synchronized sleep for power — front end powers down between coordinated
  reads via a battery sync line. Built as an addition, not a rewrite.

## 2. Scope

**In (v1):** 2-probe acquisition + averaging; multi-point linear calibration over bench UART;
continuous 0–5 V output; settable refresh rate (NVM-persisted); status LED; characterized power readout.

**Out (v2, leave seams only):** synchronized-sleep mode; deep-sleep power savings; per-rate sleep power
numbers; the battery sync line.

## 3. Hardware / pin map

| Function | Pin(s) | Notes |
|---|---|---|
| MCU | TLE9854QXW | Cortex-M0, Keil MDK + TLE985x DFP + Config Wizard |
| Probe ADC inputs | **AN7 (P2.7), AN3 (P2.3)** | ADC1, 10-bit |
| Excitation | VDDEXT → BJT current mirror → both bridges | firmware-enabled; **always on in v1**; sensor ~**0.16 mV/bar** (~160 mV FS @ 1000 bar) |
| Analog output | **PWM on P0.1** → op-amp low-pass | → 0–5 V to smart battery |
| UART (bench) | TX **P1.0** / RX **P1.1** | bench only; not downhole |
| Status LED | **P0.4** | GPIO push-pull output |
| v2 sync line | one GPIO, **reserved/unallocated** | for future battery sync |

## 4. Functional requirements

### 4.1 Acquisition
- Sample both probe channels on ADC1, oversample + average each, then average the two probes.
- RC anti-alias on the conditioned inputs + oversample-and-decimate down to the output rate (signal is slow).
- Cross-check: if the two probes differ by more than `PROBE_DISAGREE_THRESHOLD` (default ~2 % FS), raise a
  fault. (With two probes you can detect disagreement but **not** isolate which is wrong.)

### 4.2 Calibration — multi-point linear
- A bench UART command **captures a calibration point**: the operator supplies the known reference
  pressure; the device captures a *stable averaged* reading and stores the pair `(P_i, counts_i)`.
- Support **N points (≥2)**. Minimum set: ambient (absolute sensor — see below) and a high reference toward the range top; allow
  intermediate points for robustness/verification.
- Compute the transfer function by **least-squares linear fit** through the captured points → `slope`, `offset`.
- Persist `slope`/`offset` (and optionally the raw points) to **NVM**. Watchdog-safe writes; don't rewrite every boot.
- Sensor is **absolute**, zeroed at **ambient**; ambient drifts day to day (~±0.03 bar, within budget). Capture
  that day's barometric reading (available from the reference tool) for an exact zero, or use nominal ~1.013 bar absolute.

### 4.3 Output (the downhole interface)
- `pressure → analog`, linear, mapped into a **sub-range** of the 0–5 V line so the rails stay free for fault
  signaling (next bullet): pressure span **ambient → `OUT_V_LO` (default 0.5 V), 1000 bar → `OUT_V_HI`
  (default 4.5 V)**. _(Confirm endpoints with the battery. Bonus: a PWM-DAC + op-amp can't cleanly reach the
  exact rails anyway, so a sub-range sidesteps that too.)_
- Generated via **PWM on P0.1** (CCU6 recommended) → external op-amp filter.
- `PWM_FREQ_HZ` and `PWM_RESOLUTION_BITS` are **config constants — finalize once hardware confirms the
  filter.** Target ≥10-bit effective (match the ADC); `PWM_freq >> filter cutoff >> refresh rate`.
- Refresh cadence is settable (§4.5) and NVM-persisted. In v1 this is the output update rate.
- **Fault signaling on the line (one-way, device → battery):** reuse the analog line itself — on fault, drive it
  into a reserved out-of-band region (low-fault **< `OUT_V_LO`**, e.g. ≤0.25 V; high-fault **> `OUT_V_HI`**,
  e.g. ≥4.75 V). This is the standard NAMUR NE43 approach (formalized for 4–20 mA loops; the same idea maps
  onto a voltage line). The battery samples the line and reads any out-of-band value as a fault. Reserving the
  bands still leave ~1.2 bar/step at 10-bit over the full 1000-bar sensor range (proportionally finer when the `RANGE` window is narrowed to the tool's real span) — budget accuracy in bar accordingly. **Depends on the battery
  agreeing to the convention** (out-of-band = fault); if its reader can't be told that, fall back to
  hold-last-good + a bench-UART flag.
- Output must be **valid continuously** — the battery reads it asynchronously on its own clock.

### 4.4 UART — bench only
- **Interrupt-driven RX** into a ring buffer; **buffered/ISR TX**. Strictly **non-blocking** — a blocking send
  can starve WDT1 and reset the chip.
- Configure P1.0/P1.1 for the UART alternate function in Config Wizard.
- Command set (minimum): set refresh rate; capture cal point @ known pressure; compute/commit cal; read
  current pressure + raw counts; read status (fault flags); read characterized power figure for current mode.
- Simple delimited / line-based protocol with a terminator; handle RX overrun/framing + buffer overflow.
- Not a wake source; unavailable downhole.

### 4.5 Settable rate
- UART command sets the rate; value **persisted in NVM** (survives reset/power cycle).
- v1: output refresh cadence. v2: the same knob becomes the wake/read cadence.
- `REFRESH_RATE` default + allowed range: **TBD** (default likely related to the reference tool's 10 s cadence).

### 4.6 Power readout
- The chip cannot self-measure its supply current. Use a **characterized power-vs-(rate, mode) table
  hardcoded** in firmware.
- UART reports the figure for the current rate/mode. v1 holds the continuous-mode value; v2 adds per-rate
  sleep numbers.

### 4.7 Status LED (P0.4)
- Per the design notes: heartbeat (normal run), slow blink (cal armed), fast blink (capturing — "hold steady"),
  solid ~2 s (cal stored), distinct repeating pattern (fault). Local/bench indicator.

## 5. Architecture

- **Bare-metal, time-triggered super-loop — no RTOS.** A periodic timer / SysTick tick drives
  `sample → average → calibrate → output` at a fixed rate; the main loop runs cooperative, run-to-completion,
  non-blocking tasks (cal state machine, status LED, UART service, WDT1 service).
- **Operating-mode abstraction:** `MODE_CONTINUOUS` (v1) / `MODE_SYNCED_SLEEP` (v2), with the sleep/power
  policy behind a thin layer so the two modes differ **only** there.
- **Output stage decoupled:** `pressure → PWM duty` is its own module, independent of acquisition and timing
  policy, so it's unchanged whether called continuously or once per wake.
- **Reserved GPIO** for the v2 battery sync line.
- **Power table** keyed by (rate, mode).

## 6. Constraints

- **WDT1** is enabled in user mode and **cannot be disabled** — service it in the loop; keep NVM writes and
  long cal routines watchdog-safe.
- **VDDEXT** is firmware-enabled — enable and let it settle before the first sample; confirm its current limit
  covers the mirror + two bridges; **always on in v1**.
- **ADC1** is the 10-bit user ADC; ADC2 is diagnostic-only (not for the sensors).
- Use **Config Wizard** for peripheral init; pins need correct alt-function/direction (ADC inputs, PWM, UART, LED).
- 0–5 V via PWM-DAC: endpoints may not reach the exact rails.

## 7. Configurable parameters (finalize)

- `PWM_FREQ_HZ`, `PWM_RESOLUTION_BITS` — pending hardware filter values (tomorrow).
- `OUT_V_LO` / `OUT_V_HI` — voltage sub-range edges (default 0.5 V / 4.5 V). Pressure window **`range_lo`/`range_hi` (bar) is runtime-adjustable** via `RANGE` + NVM (defaults: ambient ~1 bar, 1000 bar).
- Fault-band levels (low-fault ≤ ~0.25 V, high-fault ≥ ~4.75 V) — set by the battery's convention.
- Probe ADC channels: **AN7 (P2.7) + AN3 (P2.3)** (confirmed).
- `REFRESH_RATE` default + range.
- `PROBE_DISAGREE_THRESHOLD` (~2 % FS default).

## 8. Module breakdown (for Claude Code)

- `acquisition` — ADC1 sampling, oversample, per-probe + two-probe average
- `sensors_fault` — disagreement check, fault flags
- `calibration` — multi-point capture, least-squares linear fit, apply transfer function
- `nvm_store` — cal constants + rate persistence, watchdog-safe writes
- `output_pwm` — pressure → PWM duty on P0.1
- `uart` — ISR ring buffers, command parser, status/power reporting (bench only)
- `power` — characterized table, report by (rate, mode)
- `status_led` — P0.4 patterns
- `scheduler_mode` — time-triggered loop, operating-mode abstraction, WDT1 service
- `main` — init (TLE_Init / Config Wizard, VDDEXT enable + settle), then run loop

## 9. Build order (incremental — not one-shot)

1. Project skeleton + Config Wizard setup (ADC1, PWM/CCU6, UART, P0.4, VDDEXT); confirm it flashes like BLINKY did.
2. Acquisition + averaging; verify raw counts over UART.
3. Output stage: `pressure(counts) → PWM duty → analog`; verify the 0–5 V mapping on a scope/meter (parameterize PWM freq/res).
4. Calibration: multi-point capture + least-squares fit + NVM persist; UART cal commands.
5. Settable rate + NVM persist; status LED; power-readout table.
6. Fault handling (disagreement) + startup/fail-safe (output valid only after VDDEXT settle + first reading).
7. Leave the operating-mode/sleep seams + reserved sync GPIO in place for v2.

## 10. Acceptance criteria (v1)

- Flashes and runs on the TLE9854QXW custom board.
- Both probes sampled + averaged; raw and scaled values readable over bench UART.
- Multi-point cal (≥ ambient + 1000 bar) computes a linear transfer function that persists across a power cycle.
- Continuous analog output (pressure mapped into the sub-range) tracks pressure within **±1–2 bar** vs the reference tool across the range.
- On a probe-disagreement fault, the output drives into the reserved out-of-band region (low- or high-fault).
- Refresh rate settable over UART and persists.
- UART reports the characterized power figure for the current mode.
- Status LED reflects run / cal / fault states.
- WDT1 serviced; no resets during cal or NVM writes.
- Output stays valid continuously (async-readable by the battery).

## 11. Open / TODO (the few pending)

- [ ] PWM frequency + resolution (hardware filter values — tomorrow).
- [ ] Confirm the pressure sub-range edges (`OUT_V_LO`/`OUT_V_HI`) and fault-band levels **with the battery** —
      the whole one-way fault scheme depends on its reader treating out-of-band as a fault.
- [ ] Refresh-rate default + range.
