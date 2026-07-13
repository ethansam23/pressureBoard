# TLE9854 Pressure Transmitter — Design Notes & Open Items

Captured 6 Jun 2026 — pre-PRD design review.
Board repurposes a **TLE9854QXW** (Arm Cortex-M0, 10-bit ADC1) as a 3-probe pressure
transmitter with an analog output. This is the issue/decision log that precedes the PRD.

---

## Signal chain (as understood)

VDDEXT (firmware-enabled) → BJT current mirror → 3× strain-gauge pressure probes
(2 used, 1 spare) → per-probe signal conditioning (amplified) → ADC1 channels
**AN1 (P2.1), AN7 (P2.7), AN3 (P2.3)** @ 10-bit → average / vote + scale + zero & span
calibration → **PWM on P0.1** → op-amp low-pass (PWM-as-DAC) → analog output
(**0–5 V or 0–3.3 V, TBC**) → downstream.

UART: **TX P1.0 / RX P1.1** — calibration command interface (nice-to-have, not core).

Status: **P0.4** drives an onboard debug LED → repurposed as the calibration / status indicator.

---

## Locked in (from this review)

- Front end is **conditioned / amplified**, so single-ended into ADC1 is fine — no
  per-channel instrumentation amp is needed in the firmware's assumptions.
- Three probes measure the **same pressure**; normal operation uses **two**, with the
  **third as a spare** for fault cases → a voting/redundancy scheme, not a blind 3-way average.
- **Span references exist** (known pressures / sensitivity from the datasheet). The
  **zero (offset) calibration is the hard part** and the core purpose of the board.
- Analog output range is **0–5 V or 0–3.3 V** — to be confirmed.
- **P0.4 has an onboard debug LED** — assigned as the calibration / status indicator (gives
  live feedback during the zero-capture step, which is the fiddly part).

---

## Architecture (decided 6 Jun)

**Bare-metal time-triggered super-loop — no RTOS.** An RTOS was considered and rejected for the
current scope: one periodic job plus light background, few-KB-class RAM, determinism wanted, and a
bare-metal SDK with a watchdog (WDT1) that can't be disabled. Revisit only if heavy concurrent work
is added later (full LIN/CAN stack, data logging, a display).

Structure:
- A periodic timer / SysTick tick drives the sample → vote → scale → calibrate → PWM-update path at
  a fixed rate — the deterministic control loop.
- The main loop runs cooperative tasks gated by their own intervals — cal state machine, status LED,
  UART service, WDT1 service — each run-to-completion and non-blocking.
- All I/O is non-blocking; nothing in the loop busy-waits.

### UART (P1.0 TX / P1.1 RX)
Fits the model fine with the standard event-driven pattern:
- [ ] Interrupt-driven RX into a ring buffer; TX via buffer + ISR. **No blocking/busy-wait sends** —
      a blocking UART print can starve WDT1 and reset the chip.
- [ ] Configure P1.0/P1.1 for the UART alternate function in Config Wizard (same mux lesson as the LED).
- [ ] Simple delimited / line-based command protocol with a terminator so the parser knows a command
      is complete; handle RX overrun / framing errors and ring-buffer overflow.
- [ ] Lower interrupt priority than the sample timer; add it last (nice-to-have, not core).

---

## Open decisions (the think-hard list)

### Front end / acquisition
- [ ] Conditioned output range must fit the ADC input span (0–VAREF): confirm the amp's
      output never exceeds ADC full-scale, and uses as much of it as possible.
- [ ] Conditioning gain + offset and their **tolerance** — this folds directly into
      counts-per-PSI (see span, below).
- [ ] Ratiometric vs absolute: does the conditioning/ADC reference track the excitation
      current? With a constant-current source, drift in the current or the reference shows
      up directly as pressure error.
- [ ] Anti-alias / bandwidth of the conditioned signal vs the chosen ADC sample rate.

### Redundancy logic (2 of 3)
- [ ] Which two are primary, and how is the third brought in — automatic failover on fault,
      or manual?
- [ ] Cross-check threshold: how far can the two primaries disagree before it's flagged a fault?
- [ ] Failure behavior: median/voting rule, a "probes disagree" fault flag, and what the
      analog output does when a fault is active.

### Calibration
- [ ] **Zero procedure (the crux):** requires a guaranteed 0-PSI condition (vented / known
      no-load), a *stable averaged* capture (settling time + averaging window), then store the
      offset to NVM.
- [ ] Zero trigger: UART command / button / power-up? Avoid a naive auto-zero on every boot —
      it assumes the system is genuinely at 0 PSI at that moment.
- [ ] Zero drift: thermal offset drift — re-zero on command, and/or temperature-compensate
      the zero?
- [ ] **Span, end-to-end:** the datasheet sensitivity is the *bare sensor's*; effective
      counts/PSI = sensor sensitivity × conditioning gain × ADC scale. Verify span end-to-end
      so you don't inherit the amplifier's gain tolerance — or trust the gain only if it's a
      precision part.

### Output / DAC
- [ ] Confirm output range (0–5 V vs 0–3.3 V) — sets op-amp scaling and PWM-DAC range.
- [ ] PWM-DAC sizing: which timer on P0.1 (CCU6 vs a GPT), and PWM bits vs frequency vs
      filter ripple/settling. End-to-end resolution = the **worse** of ADC effective bits and
      PWM-DAC bits — don't let the PWM be the bottleneck.
- [ ] Output load / impedance the op-amp must drive.

### System
- [ ] Target update rate / latency — drives ADC timing, averaging window, PWM update rate,
      and filter time constants.
- [ ] Startup & fail-safe behavior: enable VDDEXT → settle → confirm excitation → sample →
      output. Define the output during startup, during calibration, and on probe fault
      (a downstream-detectable level?).

### Status LED (P0.4)
Proposed single-LED scheme (active-high on P0.4) — finalize exact rates/patterns:
- [ ] Normal run: slow heartbeat blink (~every 2–3 s) so "alive" is visible, or off.
- [ ] Zero-cal armed / waiting: slow blink (~1 Hz).
- [ ] Zero-cal capturing (averaging window): fast blink (~5 Hz) = "hold at 0 PSI, don't disturb."
- [ ] Cal complete / stored: solid on ~2 s, then back to normal.
- [ ] Fault (probes disagree / out of range / cal failed): distinct repeating pattern
      (e.g. double-blink).
- [ ] Configure P0.4 as a push-pull GPIO output in Config Wizard (the LED won't drive otherwise).
- [ ] Later: mirror the same states over UART so the LED and serial status agree.

---

## Hardware / firmware constraints to honor

- **VDDEXT** is a firmware-enabled regulated output — enable it and let it settle before the
  first sample, and confirm its current limit covers the mirror + (up to) three bridges.
- **ADC1** is the 10-bit user ADC; ADC2 is an 8-bit *diagnostic* ADC for internal
  voltage/temperature supervision, not for the sensors. 10 bits = 1024 counts is the hard
  ceiling. Averaging reduces random noise (~√N) and smooths readings; it does **not** add real
  bits without genuine oversample-and-decimate plus enough noise to dither.
- **Watchdog WDT1** is enabled in user mode and **cannot be disabled while user code runs.**
  Flash/NVM writes and any long calibration sequence must service it, or the chip resets
  mid-operation. Keep the main loop and any blocking work inside the WDT1 window.
- **NVM** holds calibration (zero offset, span/temperature constants). Mind flash endurance —
  don't rewrite on every boot — and service the watchdog during writes.
- **Reference / excitation** stability is the real accuracy lever (see the ratiometric item).

---

## Next

- Draw the full signal-chain diagram off this.
- Turn the agreed picture into a PRD for Claude Code — firmware scope: VDDEXT enable +
  settle, ADC1 sampling of AN1/AN7/AN3, voting/averaging with fault handling, zero + span
  calibration with watchdog-safe NVM storage, PWM-DAC output, and an optional UART command set.
