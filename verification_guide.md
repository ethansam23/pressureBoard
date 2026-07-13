# Pressure Transmitter v1 — Verification Guide

Equipment needed:
- TLE9854QXW board, powered (5V)
- USB-UART adapter on TX P1.0 / RX P1.1, 115200 8N1
- Multimeter on P0.1 (PWM output, after op-amp filter)
- Reference pressure source + gauge (for calibration tests)
- Terminal program (PuTTY, TeraTerm, etc.)

---

## Test 1 — Boot & heartbeat

**Steps:**
1. Power on the board.
2. Watch P0.4 LED.
3. Open terminal at 115200 8N1.

**Expected:**
- [ ] LED does a short blink every ~3 seconds (100 ms on, 2900 ms off)
- [ ] Terminal shows: `== Pressure Transmitter v1 ==`
- [ ] No WDT1 resets (no repeated boot banners)
- [ ] **Boot fail-safe:** output on P0.1 sits at ~0.25 V (fault-low) from power-on
      until the first reading lands (~1 refresh period), then jumps to the live value
- [ ] No `WARN: VDDEXT not stable` and no `WARN: NVM data flash inconsistent` at boot

> **Run this test power-cycled with the J-Link DETACHED at least once** — WDT1
> is disabled in debug mode, so watchdog regressions are invisible under the
> debugger. A standalone boot loop (banner repeating, then dead after ~5
> resets = Sleep latch) is a watchdog bug.

---

## Test 2 — UART command basics

**Steps:**
1. Type `HELP` + Enter
2. Type `STATUS` + Enter

**Expected:**
- [ ] HELP prints full command list (STATUS, RAW, SCAN, AUTO, RATE, THRESH, RANGE, PROBE, OUTPUT, POWER, CAL incl. ABORT + PSI variant, PSI/BAR, HELP) — **all 19 lines, no truncation**
- [ ] Mixed-case commands work too (e.g. `Status`, `Rate 1000`)
- [ ] Typed characters are echoed; Backspace edits the line
- [ ] STATUS prints 4 lines:
  - `ProbeA: <n>  ProbeB: <n>  Avg: <n>  Probe: AVG`
  - `Output: <V>V  AUTO  Fault: no`
  - `Rate: 1000ms  Thresh: 20  Range: 1.000-1000.000 bar`
  - `Cal: NONE`
- [ ] Probe values are 0-1023 range (10-bit ADC)
- [ ] First command after power-up is NOT corrupted by a leading garbage char
- [ ] Unknown command (e.g. `FOO`) prints: `ERR: unknown 'FOO' (try HELP)`

---

## Test 3 — Acquisition & auto-print

**Steps:**
1. Type `AUTO` to enable continuous printing
2. Watch output for several cycles
3. Type `AUTO` again to disable

**Expected:**
- [ ] Prints `Auto ON` then streams a debug line every ~1 second:
  `A:<n> <mV>mV  B:<n> <mV>mV  Avg:<n>  P:uncal  Out:<V>V RAW`
- [ ] ProbeA and ProbeB values are stable (not jumping wildly)
- [ ] Avg is roughly (A + B) / 2 (or = ProbeA if `PROBE A` is set)
- [ ] `Out:` voltage matches a meter on the output; tag is `RAW` (or `CAL`/`MAN`/`FLT`)
- [ ] Prints `Auto OFF` when toggled off
- [ ] Record baseline values here: A=_____ B=_____ Avg=_____

---

## Test 4 — Output voltage (no calibration)

Without calibration active, the output maps raw counts (0-1023) linearly to duty.

**Steps:**
1. Measure voltage on P0.1 output with meter
2. Type `OUTPUT 0` — should set minimum pressure voltage
3. Type `OUTPUT 512` — should set mid-range
4. Type `OUTPUT 1023` — should set maximum pressure voltage
5. Type `OUTPUT AUTO` — resume normal operation

**Expected voltages (ideal, within filter/op-amp tolerance):**

| Command      | Expected voltage | Measured |
|-------------|-----------------|----------|
| `OUTPUT 0`   | ~0.50 V (OUT_V_LO) | _____ V |
| `OUTPUT 512` | ~2.50 V (midpoint) | _____ V |
| `OUTPUT 1023`| ~4.50 V (OUT_V_HI) | _____ V |
| `OUTPUT AUTO`| returns to live reading | _____ V |

- [ ] Voltages are monotonically increasing
- [ ] Range spans approximately 0.5 V to 4.5 V
- [ ] OUTPUT AUTO resumes live pressure-tracking output

---

## Test 5 — Fault output voltage

**Steps:**
1. Type `OUTPUT 0` then measure
2. Note: fault-low should be distinctly below normal-low

To directly test fault-low output behavior, we need probe disagreement (Test 8).
For now, verify the OUTPUT command range boundaries:

| Condition       | Expected voltage  |
|----------------|-------------------|
| Normal low      | ~0.50 V           |
| Fault low       | ~0.25 V (probe disagreement, VDDEXT loss, or boot pre-first-reading) |
| Normal high     | ~4.50 V           |
| Fault high      | ~4.75 V (not currently triggered) |

- [ ] Noted for cross-reference with Test 8

**Cleanup (IMPORTANT):** Type `OUTPUT AUTO` — otherwise the manual override
stays latched and every following test runs in MANUAL mode.

---

## Test 6 — Refresh rate (NVM persistence)

**Steps:**
1. Type `STATUS` — confirm Rate: 1000ms
2. Type `RATE 500` — set to 500 ms
3. Confirm response: `Rate=500ms (saved)`
4. Type `AUTO` — verify prints come every ~0.5 s instead of ~1 s
5. Type `AUTO` to stop
6. **Power cycle the board** (remove and restore power)
7. Type `STATUS` after reboot

**Expected:**
- [ ] Rate changes to 500 ms immediately
- [ ] AUTO prints are visibly faster (~2 per second)
- [ ] After power cycle, STATUS still shows `Rate: 500ms` (NVM persisted)
- [ ] Boot banner appears exactly once (no repeated resets)
- [ ] Out-of-range values rejected: `RATE 50` -> `ERR: rate 100-5000` (and the
      stored rate is unchanged)

**Cleanup:** Type `RATE 1000` to restore default.

---

## Test 7 — Disagree threshold (NVM persistence)

**Steps:**
1. Type `STATUS` — note current Thresh value
2. Type `THRESH 50` — set to 50
3. Confirm response: `Thresh=50 (saved)`
4. **Power cycle the board**
5. Type `STATUS` after reboot

**Expected:**
- [ ] Thresh changes to 50 immediately
- [ ] After power cycle, STATUS shows `Thresh: 50` (NVM persisted)
- [ ] Invalid values rejected: `THRESH 0` -> `ERR: thresh 1-1023`

**Cleanup:** Type `THRESH 20` to restore default.

---

## Test 8 — Fault detection (probe disagreement)

This test requires the two probes to read significantly different values.

**Method A — natural disagreement:**
If you can apply pressure to only one probe (or disconnect one), the readings will diverge.

**Method B — lower the threshold:**
1. Type `AUTO` to see live readings
2. Note the natural difference between ProbeA and ProbeB: |A - B| = _____
3. Type `THRESH <value>` where value is *less* than the observed difference

**Steps:**
1. Trigger disagreement (Method A or B)
2. Watch the LED
3. Type `STATUS`
4. Measure output voltage on P0.1

**Expected:**
- [ ] LED switches to double-blink pattern (100ms on, 100ms gap, 100ms on, 600ms pause)
- [ ] STATUS shows `Fault: YES`
- [ ] Output voltage drops to ~0.25 V (fault-low band)
- [ ] Output voltage: _____ V (should be near FAULT_V_LO = 0.25 V)

**Clear the fault:**
1. Restore probes to agreement (or raise threshold back: `THRESH 200`)
2. Verify LED returns to heartbeat
3. Verify STATUS shows `Fault: no`
4. Verify output voltage returns to normal pressure range

- [ ] Fault auto-clears when probes agree
- [ ] LED returns to heartbeat
- [ ] Output resumes normal voltage

---

## Test 9 — Calibration workflow

This is the full multi-point calibration procedure.

### 9a — Check initial state

1. Type `CAL STATUS`

**Expected:**
- [ ] Shows `Cal: NONE  pts=0` (if fresh / never calibrated)
- [ ] OR `Cal: VALID  pts=<n>  slope=<f>  offset=<f>` (if previously calibrated)

### 9b — Arm and capture point 1 (low pressure)

1. Apply known low pressure (e.g., ambient ~1 bar). Record reference: _____ bar
2. Type `CAL ARM`
3. Confirm: `Cal ARMED (0 pts)`
4. Watch LED — should be slow 1 Hz blink (cal armed)
5. Type `CAL 1.0` (or your actual ambient reading in bar)
6. Watch LED — should be fast 5 Hz blink (capturing)
7. Wait for: `Captured (1 pts)`
8. LED returns to 1 Hz blink (armed, waiting for next point)

**Expected:**
- [ ] CAL ARM response shows 0 pts
- [ ] LED: 1 Hz blink while armed
- [ ] `Capturing at 1.000 bar...` printed
- [ ] LED: 5 Hz blink during capture (~8 refresh cycles)
- [ ] `Captured (1 pts)` printed automatically
- [ ] LED: back to 1 Hz blink

### 9c — Capture point 2 (high pressure)

1. Apply a known high reference pressure (toward your configured RANGE high; use the highest you can apply). Record reference: _____ bar
2. Type `CAL <reference in bar>` (e.g. `CAL 500`)
3. Wait for: `Captured (2 pts)`

**Expected:**
- [ ] `Capturing at <your value> bar...` printed
- [ ] `Captured (2 pts)` printed

### 9d — (Optional) Capture intermediate points

Repeat for any intermediate pressures to improve fit accuracy.
Up to 8 points total supported.

### 9e — Store calibration

1. Type `CAL STORE`

**Expected:**
- [ ] `Cal stored: slope=<f> offset=<f>` printed
- [ ] LED goes solid ON for ~2 seconds, then returns to heartbeat
- [ ] Slope and offset values are reasonable:
  - slope should be positive (higher counts = higher pressure)
  - slope ~ (P_high_bar - P_low_bar) / (high_counts - low_counts) ~ _____ bar/count
  - offset ~ P_low_bar - slope * low_counts ~ _____ bar

Record: slope=_____ offset=_____

### 9f — Verify calibrated output

1. Type `STATUS` — probes should show live counts
2. Measure output voltage on P0.1
3. Calculate expected: pressure = slope * Avg + offset = _____ bar
4. Expected voltage = 0.5 + (pressure - range_lo) / (range_hi - range_lo) * 4.0 = _____ V
   (range_lo/range_hi are the RANGE window shown in STATUS)

**Expected:**
- [ ] Output voltage approximately matches calculated value
- [ ] Voltage is within the 0.5 V - 4.5 V range

---

## Test 10 — Calibration NVM persistence

**Steps:**
1. Type `CAL STATUS` — note slope and offset
2. **Power cycle the board**
3. Type `CAL STATUS` again

**Expected:**
- [ ] slope and offset are identical before and after power cycle
- [ ] `Cal: VALID` after reboot, and `pts=` shows the same point count as before
- [ ] Output voltage matches pre-reboot value at same pressure

Before: slope=_____ offset=_____
After:  slope=_____ offset=_____

---

## Test 11 — Calibration clear

**Steps:**
1. Type `CAL CLEAR`
2. Type `CAL STATUS`

**Expected:**
- [ ] `Cal cleared` printed
- [ ] CAL STATUS shows `Cal: NONE  pts=0`
- [ ] Output reverts to raw-counts mapping (uncalibrated)
- [ ] LED returns to heartbeat

---

## Test 12 — Calibration error handling

**Steps:**
1. Type `CAL 500` without arming first
2. Type `CAL ARM`, then `CAL STORE` with 0 points
3. Type `CAL ABORT` while armed

**Expected:**
- [ ] `CAL 500` without ARM: `ERR: CAL ARM first`
- [ ] `CAL STORE` with <2 points: `ERR: need >=2 pts`
- [ ] `CAL ABORT`: `Cal aborted`, returns to idle, LED heartbeat

Other distinct CAL errors (informational — only seen in the matching condition):
- 2 points captured at identical counts -> `CAL STORE` reports `ERR: degenerate fit (points at same counts)`
- 9th capture attempt -> `ERR: max 8 pts (STORE or ABORT)`
- Flash write failure -> `ERR: NVM write failed`

---

## Test 13 — Power readout

**Steps:**
1. Type `POWER`

**Expected:**
- [ ] `Power: 40 mW (continuous)` (placeholder value)

---

## Test 14 — Watchdog stability (soak)

**Steps:**
1. Leave the board running for 10+ minutes with AUTO on
2. Monitor for any resets (boot banner re-appearing)

**Expected:**
- [ ] No resets — boot banner appears only once
- [ ] Continuous auto-print output with no gaps or glitches
- [ ] LED heartbeat remains steady

---

## Test 15 — LED pattern summary

Verify each LED state was observed during testing:

| State | Pattern | Observed in test |
|-------|---------|-----------------|
| Heartbeat | 100ms on, 2900ms off | Test 1: [ ] |
| Cal armed | 500ms on, 500ms off (1 Hz) | Test 9b: [ ] |
| Cal capturing | 100ms on, 100ms off (5 Hz) | Test 9b: [ ] |
| Cal stored | Solid 2s, then heartbeat | Test 9e: [ ] |
| Fault | Double-blink (on-gap-on-pause) | Test 8: [ ] |

---

## Results summary

| Test | Description | Pass/Fail | Notes |
|------|------------|-----------|-------|
| 1 | Boot & heartbeat | | |
| 2 | UART commands | | |
| 3 | Acquisition & auto-print | | |
| 4 | Output voltage mapping | | |
| 5 | Fault output voltage | | |
| 6 | Refresh rate NVM persist | | |
| 7 | Disagree threshold NVM persist | | |
| 8 | Fault detection | | |
| 9 | Calibration workflow | | |
| 10 | Calibration NVM persist | | |
| 11 | Calibration clear | | |
| 12 | Calibration error handling | | |
| 13 | Power readout | | |
| 14 | Watchdog soak | | |
| 15 | LED patterns | | |
