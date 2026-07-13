# Pressure Transmitter — UART Command Reference

Bench command set for the TLE9854QXW downhole pressure transmitter.
_Source of truth: `app/uart_cmd.c` (`process_cmd`). Update this file when commands change._

---

## Connection

| Setting | Value |
|---|---|
| Port | TX **P1.0** / RX **P1.1** (UART2) |
| Baud | **115200**, 8N1 |
| Line ending | **Enter** (`\r` or `\n`) submits the line; trailing spaces are trimmed |
| Case | **Case-insensitive**, commands *and* arguments (`Rate 500`, `cal arm` work) |
| Editing | Input is **echoed**; **Backspace** deletes the last char |
| Boot banner | `== Pressure Transmitter v1 ==` (prints once on reset) |
| Units | Firmware is **bar-native**; CAL/RANGE accept a `PSI` suffix, `PSI`/`BAR` convert |

Unknown input returns: `ERR: unknown '<x>' (try HELP)`. A bare keyword
(`RATE`, `CAL`, …) returns that command's usage string instead.

**TEMP bring-up diagnostics** (will be removed once commissioning is done):
each boot also prints `RST 0x<hex> WFS 0x<hex> : <cause flags>` (hardware
reset cause — `PIN`/`POR`/`WDT1`/`LOCKUP`/…) and timestamped markers
(`vddext t=`, `loop t=`, `wdt-svc t=`, `refresh t=`); every NVM save prints
`nvm: <cal|set> write... rc=<code>` (0 = success).

---

## Commands at a glance

### Readouts & diagnostics
| Command | Description |
|---|---|
| `STATUS` | Live readings + settings (4 lines, see below) |
| `RAW` | One fresh burst: `avg/min/max/mV/valid` per probe channel |
| `SCAN` | Sweep all analog inputs (AN0/AN1/AN2/AN3/AN7) — `cnt` + `mV` |
| `AUTO` | Toggle the **debug stream** (per-probe counts+mV, pressure, output V, drive state) each refresh |
| `POWER` | Power readout (`40 mW`, placeholder until characterized) |
| `PSI <x>` | Convert: prints `<x> psi = <y> bar` |
| `BAR <x>` | Convert: prints `<x> bar = <y> psi` |
| `HELP` | On-device command list |

### Settings — persisted to NVM
| Command | Range / values | Description |
|---|---|---|
| `RATE <ms>` | 100–5000 ms | Refresh / output update rate (out-of-range or non-numeric → `ERR: rate 100-5000`) |
| `THRESH <cnt>` | 1–1023 counts | Probe-disagreement fault threshold (~thresh/8 hysteresis on clear) |
| `RANGE <lo> <hi> [PSI]` | `0 ≤ lo < hi ≤ 1000` bar, span ≥ 1 | Output pressure window: `lo`→0.5 V, `hi`→4.5 V. `PSI` suffix converts both operands |
| `PROBE A\|B\|AVG` | `A`, `B`, `AVG` (or `DUAL`) | Probe source select |

All numeric arguments are strictly validated — trailing garbage (`RATE 1000x`,
`OUTPUT 10O`) is rejected instead of silently misparsed. Commands are processed
at most **one per loop pass**, so a pasted batch executes sequentially.

### Output control
| Command | Description |
|---|---|
| `OUTPUT <n>` | **Manual override** — pin output to raw count `n` (0–1023). **Latches** — ignores live pressure until cleared. |
| `OUTPUT AUTO` | Clear override, resume live tracking |

### Calibration — values in **bar** (or append `PSI`)
| Command | Description |
|---|---|
| `CAL ARM` | Start a calibration session |
| `CAL <bar>` | Capture a point at the given reference pressure (must be armed; `0 < bar ≤ 1000`; **max 8 points** — at the cap: `ERR: max 8 pts (STORE or ABORT)`) |
| `CAL <x> PSI` | Same, but the value is in **psi** (converted at the parser; echo shows both units) |
| `CAL STORE` | Compute least-squares fit + save to NVM. Distinct errors: `ERR: need >=2 pts`, `ERR: degenerate fit (points at same counts)`, `ERR: NVM write failed` |
| `CAL STATUS` | Show `VALID/NONE`, point count, slope, offset (points persist across power cycles) |
| `CAL CLEAR` | Erase the stored calibration (on erase failure: `Cal cleared (RAM only - ...)`) |
| `CAL ABORT` | Abort the current session |

**Timing:** each capture averages `8 × RATE` ms of readings (8 s at the default
1000 ms — set `RATE 100` during bench cal for ~0.8 s captures, restore after).
`CAL STORE` itself takes ~10 ms; the 2 s solid LED afterward is the *stored*
indicator, not store-in-progress. While **capturing**, `CAL ARM`/`CAL STORE`/
another `CAL <bar>` are rejected (`ERR: capture in progress…`) so the in-flight
point can't be silently dropped — and capture **pauses while any fault is
active** so a corrupted reading can't enter a cal point.

---

## Output formats

**`STATUS`** — four lines:
```
ProbeA: <a>  ProbeB: <b>  Avg: <c>  Probe: <A|B|AVG>
Output: <V>V  <AUTO|MANUAL>  Fault: <YES|no>
Rate: <ms>ms  Thresh: <t>  Range: <lo>-<hi> bar
Cal: <NONE | VALID  slope=<f> offset=<f>>
```

**`AUTO`** — one line per refresh (`FLT`/`MAN`/`CAL`/`RAW` = which path drives the output):
```
A:<cnt> <mV>mV  B:<cnt> <mV>mV  Avg:<cnt>  P:<bar>bar  Out:<V>V <state>
```
`P:` shows `uncal` until a calibration is stored. `Out:` is the *actual* commanded output voltage.

**`RAW`** (1 LSB ≈ 5 mV on the attenuated P2.x inputs):
```
RAW (P2.x inputs, 1 LSB ~5mV):
  A: avg=<> min=<> max=<> mV=<> valid=<>/16
  B: avg=<> min=<> max=<> mV=<> valid=<>/16
```

**`SCAN`** (`<-` marks the channels the firmware uses):
```
SCAN all analog inputs (<- = used by firmware):
  AN0(P2.0)    cnt=<> mV=<>
  AN1(P2.1)    cnt=<> mV=<>
  AN2(P2.2)    cnt=<> mV=<>
  AN3(P2.3) <-B cnt=<> mV=<>
  AN7(P2.7) <-A cnt=<> mV=<>
```

**`CAL STATUS`**:
```
Cal: <VALID|NONE>  pts=<n>  slope=<f>  offset=<f>     (slope/offset only when VALID)
```

Settings echo `... (saved)` on NVM-write success, or `(NVM write failed)`.

---

## Output behavior (how the voltage is computed)

The output is a PWM-DAC on **P0.1** → op-amp filter → 0.5–4.5 V analog line (fault bands below/above).

In **priority order** (higher rows override lower ones):

| Condition | Output |
|---|---|
| **Any fault active** (probe disagreement, VDDEXT/excitation unstable, or ADC stalled) | **Fault-low ≈ 0.25 V** — overrides everything, including manual (`OUTPUT <n>` during a fault latches and answers `(latched; fault active...)`) |
| Manual override (`OUTPUT <n>`) | Fixed at `0.5 + (n/1023)×4.0` V (re-asserts itself after a fault clears) |
| Calibrated (cal valid) | `0.5 + (bar − range_lo)/(range_hi − range_lo) × 4.0` V (keeps driving even while a new cal session is armed) |
| Uncalibrated | `0.5 + (Avg / 1023) × 4.0` V (raw counts) |

`Avg` is the probe source selected by `PROBE` (A only, B only, or the average).

**Boot fail-safe:** from reset until the first completed reading (~1 refresh
period), the line is held at **fault-low ≈ 0.25 V** so the battery can't
mistake a not-yet-measured boot value for a real ambient pressure.

---

## Common workflows

**Single-probe board (probe on AN7):**
```
PROBE A          → output follows ProbeA; averaging + disagreement fault disabled
STATUS           → confirm "Probe: A"
```

**Set the operating window (better resolution):**
```
RANGE 0 600      → map 0–600 bar onto 0.5–4.5 V  (≈0.73 bar/step vs 1.2 at full 1000 bar)
```

**Calibrate (multi-point; psi gauge on the bench):**
```
RATE 100         → fast captures (~0.8 s each) for the bench session
CAL ARM
CAL 14.7 PSI     → capture at ambient ("Capturing at 1.013 bar (14.700 psi)...")
CAL 7250 PSI     → capture at 500 bar reference
CAL STORE        → "Cal stored: slope=.. offset=.."
CAL STATUS       → verify VALID
RATE 1000        → restore the operating rate
```

**Test the output stage independent of the sensor:**
```
OUTPUT 0         → expect ~0.50 V (~10% duty)
OUTPUT 512       → expect ~2.50 V (~50% duty)
OUTPUT 1023      → expect ~4.50 V (~90% duty)
OUTPUT AUTO      → resume live  (IMPORTANT: forgetting this pins the output)
```

---

## Quirks & gotchas

- **`OUTPUT <n>` latches.** It stays pinned (a stuck `OUTPUT 1023` reads as a constant ~90% duty) until `OUTPUT AUTO`. `STATUS` now shows `MANUAL` vs `AUTO` so you can spot it, and the `AUTO` stream tags it `MAN`. A fault temporarily forces fault-low; the manual value re-asserts when the fault clears.
- **`Fault: YES` covers two sources:** probe disagreement *and* excitation (VDDEXT) instability. The VDDEXT check runs every refresh and auto-recovers; while it's down you'll also have seen `WARN: VDDEXT not stable (excitation down)` at boot.
- **Re-calibrate after the bar firmware.** The calibration NVM magic was bumped (cal is now stored in bar), so any pre-bar calibration is rejected — `CAL STATUS` will read `NONE` until you re-cal.
- **Settings survive power cycles** (`RATE`/`THRESH`/`RANGE`/`PROBE`); the calibration is separate NVM. Captured cal points are reloaded too, so `CAL STATUS` shows the real `pts=` count after a reboot.
- **`RANGE` only affects the calibrated output path.** Uncalibrated output maps raw counts 0–1023 directly to 0.5–4.5 V regardless of `RANGE`.
- **Commands and arguments are both case-insensitive** (`Rate 500`, `cal arm`, `output Auto` all work).
- If boot prints `WARN: NVM data flash inconsistent (saves disabled)`, the data-flash mapping failed its startup check — all NVM saves return `(NVM write failed)` / `ERR: NVM write failed` until the data sector is recovered (full chip erase + reflash).

---

## Changelog

- **2026-06-10:** PSI front end — `CAL <x> PSI`, `RANGE <lo> <hi> PSI`, and `PSI <x>`/`BAR <x>` converters (firmware stays bar-native). Round-2 verified fixes: removed the double WDT1 window-count (the standalone boot-reset loop); NVM saves are now a single mapped-page write (power-fail-safe, no pre-erase) with `nvm: … rc=` diagnostics; fault also raised on ADC stall; VDDEXT supervision re-enables a latched-off regulator; ~thresh/8 fault hysteresis; capture pauses during faults and CAL ARM/STORE/<bar> are rejected mid-capture; CAL CLEAR reports erase failure honestly; failed STORE keeps the previous fit live; strict numeric parsing everywhere; one command per loop pass; bare keywords print usage; trailing spaces trimmed; TEMP boot/reset-cause diagnostics added.
- **2026-06-09:** fix batch from the doc-vs-firmware audit — RATE now rejects out-of-range (`ERR: rate 100-5000`) instead of persisting an unclamped value; arguments case-insensitive (`Rate 500` works); `OUTPUT` rejects non-numeric args; CAL STORE errors are now distinct (need >=2 pts / degenerate fit / NVM write failed); `CAL <bar>` at the 8-point cap errors instead of silently doing nothing; CAL ABORT added to on-device HELP; cal points persist across power cycles; boot output is fault-low until the first reading; fault (incl. new VDDEXT supervision) overrides manual; TX buffer 1024 B (HELP no longer truncates).
- _(add dated entries here as commands change — e.g. "2026-06-07: added PROBE, RANGE; CAL now in bar")_
