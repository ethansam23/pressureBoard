# Pressure Transmitter — UART Command Reference

Bench command set for the TLE9854QXW downhole pressure transmitter (digital-link
firmware). _Source of truth: `app/uart_cmd.c` (`process_cmd`). Update this file
when commands change._

---

## The shared line — read this first

There is **one UART** (TX **P1.0** / RX **P1.1**, UART2, **9600 8N1**) and it is
owned by the **downhole packet stream** (wire format: `link_protocol.md`). The
console shares it under strict **mutual exclusion**:

- **At power-on the console is LOCKED**: the firmware transmits *binary packets
  only* — no banner, no text — and every command line is silently ignored.
- Send exactly **`CONSOLE UNLOCK`** (+ Enter) to open a bench session. The
  packet stream **suspends** (after the in-flight packet completes atomically)
  and the console takes the line: banner + boot reset cause print, commands
  work normally.
- **`CONSOLE LOCK`**, **5 minutes of RX inactivity**, or a **power cycle**
  re-locks: queued console output drains, the line goes idle for a full
  inter-packet gap, then the packet stream resumes.
- **Production builds (`LINK_CONSOLE_EN=0`) have no console at all** — parser
  and console TX are compiled out; packets are the only possible bytes.
  Pre-deployment check: send `CONSOLE UNLOCK`; silence = production build.

While unlocked, the wire carries **no packets** — a connected logger sees the
line go quiet (its staleness handling applies). The line carries either packets
or text, never both interleaved.

## Connection

| Setting | Value |
|---|---|
| Port | TX **P1.0** / RX **P1.1** (UART2) — same physical line as the logger |
| Baud | **9600**, 8N1 |
| Line ending | **Enter** (`\r` or `\n`) submits; trailing spaces trimmed |
| Case | **Case-insensitive**, commands *and* arguments (`Rate 500`, `cal arm`) |
| Editing | Echo + Backspace — **only while unlocked** (locked = totally silent) |
| Unlock banner | `== Pressure Transmitter v2 (digital link) ==`, stream-suspended notice, boot reset cause (`RST 0x… WFS 0x…` + `[WDT1]/[POR]/[PIN]`), NVM-health warning if applicable |
| Units | Firmware is **bar-native**; CAL accepts a `PSI` suffix; `PSI`/`BAR` convert |

Unknown input returns `ERR: unknown '<x>' (try HELP)`. A bare keyword
(`RATE`, `CAL`, …) returns that command's usage string.

**TEMP diagnostics** (slated for removal): every NVM save prints
`nvm: <cal|set> write... rc=<code>` (0 = success).

---

## Commands at a glance

### Session
| Command | Description |
|---|---|
| `CONSOLE UNLOCK` | Open a bench session (suspends the packet stream). The **only** line recognized while locked |
| `CONSOLE LOCK` | End the session, resume the packet stream (also: 5-min inactivity, power cycle) |

### Readouts & diagnostics
| Command | Description |
|---|---|
| `STATUS` | Live readings, link state, fault causes, settings (5 lines, below) |
| `RAW` | One fresh burst: `avg/min/max/mV/valid` per probe channel — **native 10-bit** units (production pipeline counts are 4×) |
| `SCAN` | Sweep all analog inputs (AN0/AN1/AN2/AN3/AN7) — `cnt` + `mV` (native 10-bit) |
| `AUTO` | Toggle the debug stream (counts+mV per probe, pressure, pending link code + tag) each refresh. Cleared on re-lock |
| `POWER` | Power readout (`40 mW`, placeholder until characterized) |
| `PSI <x>` / `BAR <x>` | Unit converters |
| `HELP` | On-device command list |

### Settings — persisted to NVM
| Command | Range / values | Description |
|---|---|---|
| `RATE <ms>` | 100–5000 ms | Sample/refresh rate (the stream repeats the latest value at 25 pkt/s regardless) |
| `THRESH <cnt>` | 1–4092 counts (12-bit-scaled) | Probe-disagreement threshold (~thresh/8 hysteresis on clear) |
| `PROBE A\|B\|AVG` | `A`, `B`, `AVG` (or `DUAL`) | Probe source select |

All numeric arguments are strictly validated — trailing garbage (`RATE 1000x`)
is rejected. Commands run at most **one per loop pass**, so a pasted batch
executes sequentially. NVM saves are additionally gated by the link fence — a
failed fence reports `NVM write failed` and leaves stored settings untouched.

### Link test
| Command | Description |
|---|---|
| `LINKTEST <n>` | Force 16-bit code `n` (0–65535) onto the wire — **overrides live values AND fault codes**. RAM-only, **auto-expires after 5 min**. The stream is suspended while unlocked, so: set the code, then `CONSOLE LOCK` to actually transmit it |
| `LINKTEST OFF` | Return the wire to live values |
| `SIM BAR` | Drive the wire from the synthetic profile, bypassing the ADC and calibration. Index resets to 0 |
| `SIM COUNTS` | Feed the profile in as synthetic ADC counts; the real calibration + encode path runs on the way out |
| `SIM OFF` | Return to real acquisition |
| `SIM PHASE A\|B\|FULL` | Pick the resolution sweep, one ladder cycle, or the whole 24 h run. Index resets to 0 |
| `SIM SEEK <n>` | Jump to refresh index `n` (wraps at the phase length) |
| `SIM STATUS` | Mode, phase, index/length, active RATE, current segment, current code |

`STATUS` shows `TEST(!)` while an override is active. **Never deploy with
LINKTEST active** — the 5-min expiry and the power-cycle reset are backstops,
not the plan.

### `SIM` — synthetic pressure source (bench builds only)

Present only when the firmware is built with `-DAPP_ENABLE_SIM=1`. Substitutes
the transducers so the link path can be verified in isolation from the ADC and
the calibration math. `STATUS` shows `SIM(!)` while it is active.

The profile has two phases (full detail in `verification_guide.md`):

- **Phase A — resolution sweep, 20,600 s.** 0 → 10000 → 0 deci-bar at one code
  per refresh, so every one of the 10,001 valid codes is transmitted once
  ascending and once descending.
- **Phase B — ramp-timing ladder, 3,600 s per cycle.** Five tiers of
  fixed-duration ramp windows (5 min, 2 min, 1 min, 30 s, 10 s), four ramps
  each, spanning 1 to 1000 dbar/refresh.
- **`FULL`** = A + 18 × B + a 1,000 s stop = exactly 24 h.

Points that bite if you skip them:

- **Set it, then `CONSOLE LOCK`.** Packets are suspended while the console is
  unlocked, exactly like `LINKTEST`.
- **Window durations are wall-clock milliseconds, converted to refresh counts
  against the *current* `RATE` on every refresh.** Do not change `RATE` while a
  run is in progress: the profile length changes with it, so the current index
  lands somewhere else entirely and the wire jumps. The reference stream you
  are scoring against is generated for one `RATE` and is invalid after such a
  change. (It is at least detectable — the verifier reports it as a divergence
  at the moment of the change.) Set `RATE` first, then arm `SIM`.
- **Unlike `LINKTEST` there is no auto-expiry.** A 24-hour soak has to keep
  running. Sim state is RAM-only, so a reset returns the board to real
  acquisition — which is also how a mid-soak reset becomes visible.
- **Genuine ADC and excitation faults still win** in both modes, so a rig
  problem can never be masked by synthetic data.
- **`SIM COUNTS` is not exact and must not be verified as one.** One
  12-bit-scaled count is ~2.4 dbar against the 0.1 dbar wire LSB, so the
  profile's 1 dbar steps quantise into a staircase. Only `SIM BAR` is checked
  value-exact.

### Calibration — values in **bar** (or append `PSI`)
| Command | Description |
|---|---|
| `CAL ARM` | Start a calibration session |
| `CAL <bar>` | Capture a point at the reference pressure (armed; `0 < bar ≤ 1000`; **max 8 points**) |
| `CAL <x> PSI` | Same, value in **psi** (converted at the parser; echo shows both) |
| `CAL STORE` | Least-squares fit + save to NVM. Distinct errors: `ERR: need >=2 pts`, `ERR: degenerate fit…`, `ERR: NVM write failed` |
| `CAL STATUS` | `VALID/NONE`, point count, slope, offset (points persist across power cycles) |
| `CAL CLEAR` | Erase stored calibration (on erase failure: `Cal cleared (RAM only - …)`) |
| `CAL ABORT` | Abort the current session |

**Timing:** each capture averages `8 × RATE` ms of readings (8 s at the default
1000 ms — set `RATE 100` during bench cal for ~0.8 s captures, restore after).
While **capturing**, `CAL ARM`/`CAL STORE`/another `CAL <bar>` are rejected so
the in-flight point can't be dropped — and capture **pauses while any fault is
active** so a corrupted reading can't enter a cal point.

---

## Output formats

**`STATUS`** — five lines:
```
ProbeA: 2048  ProbeB: 2052  Avg: 2050  Probe: AVG
Link: 0x04D2  LIVE  mode=CONSOLE (stream suspended)  pkts=12345 aborts=0 skips=0
Faults: none
Rate: 1000ms  Thresh: 80  NVM: ok
Cal: VALID  slope=0.245 offset=-1.013
```
- `Link:` the 16-bit code on (or pending for) the wire; `LIVE` or `TEST(!)`,
  plus `SIM(!)` when the synthetic source is driving
- `mode=` `PKT` (streaming) or `CONSOLE (stream suspended)`
- `Faults:` **all** active causes (`ADC_STALL` / `VDDEXT` / `DISAGREE`) — the
  wire carries only the highest-priority one
- `pkts/aborts/skips`: packets sent, aborts (any origin), busy-skips

**`AUTO`** — one line per refresh; tag = `TST`/`FLT`/`CAL`/`UNC`:
```
A:2048 2500mV  B:2052 2505mV  Avg:2050  P:123.400bar  Link:0x04D2 CAL
```
`P:` shows `uncal` until a calibration is stored. Counts are 12-bit-scaled
(0–4092).

**`RAW`** (native 10-bit; 1 LSB ≈ 5 mV on the attenuated P2.x inputs):
```
RAW (native 10-bit, 1 LSB ~5mV; production counts = 4x):
  A: avg=<> min=<> max=<> mV=<> valid=<>/16
  B: avg=<> min=<> max=<> mV=<> valid=<>/16
```

**`SCAN`** — unchanged (`<-` marks the channels the firmware uses).

Settings echo `… (saved)` on NVM-write success, or `(NVM write failed)`.

---

## Wire behavior (how the link code is computed)

The downhole interface is a one-way 9600-baud packet stream on P1.0 (full
spec: `link_protocol.md`). Each packet carries one 16-bit code, selected in
**priority order** every refresh:

| Condition | Code |
|---|---|
| `LINKTEST` active | the forced code (overrides everything) |
| ADC stalled | `0xFF04` ADC_STALL |
| `SIM BAR` active | the profile code — but ADC_STALL and VDDEXT above still win |
| VDDEXT unstable | `0xFF05` VDDEXT |
| Probe disagreement | `0xFF03` DISAGREE |
| Calibrated | pressure, 0.1 bar/LSB (`0`–`10000`); >1010 bar → `0xFF06` OVER_RANGE, <−5 bar → `0xFF07` UNDER_RANGE |
| Uncalibrated | `0xFF02` UNCAL — **never raw counts dressed as pressure** |

**Boot fail-safe:** the stream starts within milliseconds of power-on carrying
`0xFF01` NO_READING until the first refresh completes — the wire never shows a
fake pressure, and a silent line means a dead tool (logger staleness handling).

---

## Common workflows

**Open a session / close it:**
```
CONSOLE UNLOCK    → banner; stream suspended
...work...
CONSOLE LOCK      → stream resumes (or just walk away: 5-min auto-relock)
```

**Single-probe board (probe on AN7):**
```
PROBE A           → readings follow ProbeA; disagreement fault disabled
STATUS            → confirm "Probe: A"
```

**Calibrate (psi gauge on the bench):**
```
RATE 100          → fast captures (~0.8 s each)
CAL ARM
CAL 14.7 PSI      → capture at ambient
CAL 7250 PSI      → capture at 500 bar reference
CAL STORE         → "Cal stored: slope=.. offset=.."
RATE 1000         → restore operating rate
CONSOLE LOCK      → resume stream; watch decoded pressure in the monitor
```

**Test the link end-to-end (independent of the sensor):**
```
LINKTEST 10000    → full scale: expect 7F .. 27 10 C8 on the wire
CONSOLE LOCK      → stream transmits the forced code
(scope/monitor)   → verify
CONSOLE UNLOCK
LINKTEST OFF      → back to live
```

---

## Quirks & gotchas

- **Everything is silent until `CONSOLE UNLOCK`.** A board that "prints
  nothing at boot" is CORRECT — watch the binary stream instead (host monitor
  or scope). If `CONSOLE UNLOCK` also gets no reply, you have a production
  build (or the wrong baud — it's 9600 now, not 115200).
- **Unlocking stops the stream.** A logger connected during a bench session
  records its staleness/no-data behavior for that period. That's by design —
  text and packets never share the wire.
- **`LINKTEST` latches (5-min cap).** `STATUS` shows `TEST(!)`; `AUTO` tags
  `TST`. It overrides fault codes too — that's the point (it tests the link),
  and why it expires.
- **Re-calibrate + re-enter settings after flashing this firmware.** Both NVM
  magics were bumped (12-bit count scale; window field removed): old cal and
  old settings are intentionally rejected. Wire shows `0xFF02` until re-cal.
- **`Faults:` in STATUS lists every active cause; the wire shows one** (the
  highest priority). A VDDEXT fault can hide a simultaneous disagreement on
  the wire — STATUS is the full picture.
- **RAW/SCAN are native 10-bit; the production pipeline is 4× those counts.**
  A RAW avg of 512 corresponds to Avg≈2048 in STATUS/AUTO.
- If the unlock banner includes `WARN: NVM data flash inconsistent`, all NVM
  saves will fail until the data sector is recovered (full erase + reflash).

---

## Changelog

- **2026-08-26: `SIM` added (bench builds only).** Synthetic pressure profile
  standing in for the transducers, for the 24 h link-path soak: a full-range
  resolution sweep (all 10,001 codes) plus a fixed-duration ramp-timing ladder
  (5 min down to 10 s windows, 1–1000 dbar/refresh). `SIM
  OFF|BAR|COUNTS|PHASE|SEEK|STATUS`; `STATUS` gains a `SIM(!)` marker. Compiled
  out entirely unless built with `-DAPP_ENABLE_SIM=1`; never persisted to NVM.

- **2026-07-14: DIGITAL LINK REARCHITECTURE.** Analog PWM-DAC output (P0.1)
  removed entirely; the downhole interface is now a one-way 9600-baud packet
  stream on P1.0 (see `link_protocol.md`). Console moved to the same line at
  9600 under mutual exclusion: boots LOCKED, `CONSOLE UNLOCK`/`CONSOLE LOCK`
  added, 5-min inactivity auto-relock, banner moved to unlock (boot is
  packets-only). Removed: `OUTPUT` (→ `LINKTEST <n>|OFF`, auto-expiring),
  `RANGE` (wire scale is fixed absolute 0–1000 bar). STATUS/AUTO reformatted
  (link code, mode, packet counters, per-cause fault list, NVM health).
  Counts are 12-bit-scaled (THRESH 1–4092; RAW/SCAN stay native 10-bit).
  Both NVM magics bumped — settings + calibration reset on first boot.
  Fault causes split (ADC_STALL / VDDEXT / DISAGREE) and reported as distinct
  wire codes. NVM writes fenced to wire-idle windows (fail-closed).
- **2026-06-10:** PSI front end — `CAL <x> PSI`, `RANGE <lo> <hi> PSI`, and `PSI <x>`/`BAR <x>` converters (firmware stays bar-native). Round-2 verified fixes: removed the double WDT1 window-count (the standalone boot-reset loop); NVM saves are now a single mapped-page write (power-fail-safe, no pre-erase) with `nvm: … rc=` diagnostics; fault also raised on ADC stall; VDDEXT supervision re-enables a latched-off regulator; ~thresh/8 fault hysteresis; capture pauses during faults and CAL ARM/STORE/<bar> are rejected mid-capture; CAL CLEAR reports erase failure honestly; failed STORE keeps the previous fit live; strict numeric parsing everywhere; one command per loop pass; bare keywords print usage; trailing spaces trimmed; TEMP boot/reset-cause diagnostics added.
- **2026-06-09:** fix batch from the doc-vs-firmware audit — RATE now rejects out-of-range (`ERR: rate 100-5000`) instead of persisting an unclamped value; arguments case-insensitive (`Rate 500` works); `OUTPUT` rejects non-numeric args; CAL STORE errors are now distinct (need >=2 pts / degenerate fit / NVM write failed); `CAL <bar>` at the 8-point cap errors instead of silently doing nothing; CAL ABORT added to on-device HELP; cal points persist across power cycles; boot output is fault-low until the first reading; fault (incl. new VDDEXT supervision) overrides manual; TX buffer 1024 B (HELP no longer truncates).
