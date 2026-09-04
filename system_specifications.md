# System Specifications & Statistics — Downhole Pressure Transmitter

_Compiled 2026-07-15 from: TLE9854QXW datasheet (Rev 1.2), Keller 7LHP
datasheet (07/2025), the design notes, the firmware source, and the step-4b
build record. Items the paper trail cannot prove are marked **UNVERIFIED**
with the test that resolves them. Firmware-level wire detail lives in
`link_protocol.md`; requirements in `TLE9854_pressure_transmitter_PRD_rev2.md`._

---

## 1. System at a glance

```
 Keller 7LHP probes (×3 fitted, 2 used + 1 spare)
        │  piezoresistive bridges, current-excited
 VDDEXT (5V) → BJT current mirror → bridges → per-probe amplifier ("tunable gain")
        │                                            │ single-ended
        │                                     ADC1 CH12 (P2.7) / CH9 (P2.3)
        │                                            │ 10-bit SAR
 TLE9854QXW  ── 16× oversample ÷4 → 12-bit-scaled counts → linear cal (float)
  Cortex-M0                                          │ bar
  @ 40 MHz    ── deci-bar encode ── one-way UART packet stream ── P1.0 @ 9600
                                                     │
                                          battery/logger (+ bench tools)
```

| Headline stat | Value |
|---|---|
| Pressure range (wire scale, fixed absolute) | 0 – 1000.0 bar, 0.1 bar/LSB |
| Sample rate (measurement) | 1 Hz default, settable 0.2 – 10 Hz |
| Transmission rate | ~9.1 packets/s (each reading sent ~9× at 1 Hz refresh) |
| Accuracy target | ±1–2 bar after calibration @ ref temp — **UNVERIFIED** (gain + noise, bench Test 9) |
| Resolution (ADC path) | ~0.24 bar/count at 12-bit-scaled (~0.98 bar native) |
| Flash / RAM usage | 24.5 KB of 64 KB (38 %) / ~2.2 KB of 4 KB (54 %) |

## 2. MCU platform — Infineon TLE9854QXW

| Parameter | Value | Source |
|---|---|---|
| Core / clock | Arm Cortex-M0, f_SYS = CPU = peripheral clock = **40 MHz** | datasheet §clocks |
| Memory | **64 KB flash** (48–96 KB family), **4 KB RAM**, 24 KB BootROM | datasheet |
| Package | VQFN-48 (7×7 mm), AEC-qualified, green | datasheet |
| Supply | VS = 5.5–28 V (extended 3–28 V) | datasheet |
| Junction temp | T_j −40 … **+175 °C** | datasheet |
| VDDEXT | 5.0 V LDO output, **40 mA** limit — powers the sensor excitation | datasheet pinout |
| Watchdog | WDT1, ~300 ms service budget; 5 consecutive resets latch Sleep Mode | firmware/UM |
| Data flash (NVM) | 4 KB MapRAM sector, 128 B pages; **page program ~3 ms typ, page erase ~4 ms typ** (basis of the firmware's 5–10 ms IRQ-masked fence budget) | datasheet |
| Aux ADC (ADC2) | 8-bit, supervision-only — includes **two die-temperature channels (T_SENSE1/2)**: a candidate temperature source for the future compensation LUT | datasheet |

## 3. Sensor — Keller 7LHP (0–1000 bar variant)

| Parameter | Value |
|---|---|
| Technology | Piezoresistive bridge, oil-filled 316L/Inconel/Ti capsule, ø15×8 mm, ~8.6 g |
| Range / proof pressure | 0–1000 bar abs / **2200 bar proof** |
| Sensitivity | 0.12–**0.16 typ**–0.20 **mV/(mA·bar)** — current-excited; the board's nominal "0.16 mV/bar" implies **≈1 mA bridge excitation** through the mirror (**UNVERIFIED** — confirm current on bench) |
| Full-scale bridge signal | ≈160 mV @ 1 mA, 1000 bar |
| Accuracy @ 20–25 °C | **±0.25 %FS typ (±2.5 bar), ±0.50 %FS max** (BFSL nonlinearity + hysteresis + repeatability) |
| Long-term stability | ±0.25 %FS/year |
| Offset | <±25 mV/mA uncompensated; <±2 mV/mA with compensation resistors |
| **Compensated temp range** | **−10…+80 °C standard** (options to −55…180 °C) — outside it, thermal error grows: the driver for the planned on-board temperature LUT |
| Media/ambient/storage temp | −40…150 °C |
| Endurance | >10⁷ pressure cycles; 10 g vibration (10–2000 Hz); <50 g shock; resonance >30 kHz |
| Fitted | 3 probes measuring the same pressure; firmware uses 2 (A=AN7/P2.7, B=AN3/P2.3) with disagreement cross-check; 1 spare |

## 4. Acquisition chain & ADC statistics

**The chain:** VDDEXT → BJT mirror (≈1 mA/bridge) → bridge (µV–mV signal) →
per-probe amplifier (**gain ~30× inferred** to span the ADC; user-tunable —
**UNVERIFIED on paper**; measured counts/bar comes from bench Test 9; any gain
retune invalidates calibration) → single-ended into ADC1.

**ADC1 hardware:** 10-bit SAR, 12 channels; conversion = **17 ADC-clock
cycles**; analog clock f_ADCI ≤ 5 MHz → **≈3.4 µs per conversion** at the
limit. Total unadjusted error ±10 LSB (calibrated). Firmware observes EOC
within 1–2 poll iterations (≤~5 µs), consistent. Input scaling on P2.x ≈
5 V full scale → **1 native LSB ≈ 4.9 mV ≈ 0.98 bar** (at nominal gain).

**How sampling actually runs (per refresh, default 1 Hz):**

| Step | Count | Time |
|---|---|---|
| Mux-settle discard per channel | 1 × 2 ch | ~10 µs |
| Oversample conversions | 16 × 2 ch = 32 | each ≈3.4 µs conv + ~2–5 µs polling overhead (-O0) |
| **Total burst** | **34 conversions** | **≲0.3 ms healthy**; hard-bounded 34 ms if the ADC stalls (1 ms EOC guard/conversion — the fenced worst case) |
| Averaging | sum/4 (NOT /16) | keeps 2 oversampling bits → **12-bit-scaled counts 0–4092** |

So: instantaneous conversion rate during a burst is ~100–300 kS/s-class, but
the *measurement* rate is one filtered reading per channel per refresh —
**1 Hz default (settable 100–5000 ms)**. Per-sample fallback substitution
(last-good) on EOC timeout/invalid flag; ≥16 fallbacks in a burst = ADC_STALL
fault. "12 effective bits" requires ≥1 native LSB of noise dither —
**UNVERIFIED** until the bench RAW capture (Test 9).

## 5. Processing pipeline (per refresh)

1. Acquire A, B → combined per probe mode (A/B/average).
2. Supervision: ADC stall + VDDEXT stability → distinct fault causes.
3. Disagreement check: |A−B| > threshold (default 80 counts ≈ 2 %FS,
   NVM-settable 1–4092, ~thresh/8 hysteresis).
4. Calibration: `bar = slope×counts + offset` (IEEE-754 single, softfloat;
   multi-point least-squares fit, up to 8 points, NVM-persisted).
5. Encode: deci-bar 0–10000, clamps −5…0→0 / 1000–1010→10000, non-finite →
   UNCAL; priority ladder ADC_STALL > VDDEXT > DISAGREE > UNCAL > pressure.
6. Latency sample→wire: ≤ one refresh period + ≤110 ms packet slot
   (**~1.11 s worst at 1 Hz**; ~210 ms at 10 Hz).

## 6. Output link (summary — normative detail in `link_protocol.md`)

| Stat | Value |
|---|---|
| Physical | UART 9600 8N1 on P1.0, one-way, 5 V push-pull, high-Z in reset (harness pull-up required) |
| Packet | `0x7F` + gap + MSB/LSB/checksum; total gap+4.17 ms = 7.1–9.2 ms (<10 ms spec) |
| Cadence | 110 ms period (~9.1 pkt/s); idle ≥21.8 ms guaranteed (~100.8 ms nominal) |
| Payload | 16-bit big-endian: 0–10000 deci-bar; 0xFF01–07 status/fault codes |
| Availability | worst valid-packet gap ≈145 ms (fenced ADC-stall); ≈8.7 pkt/s sustained under fault; supports logger ≤~4–4.5 Hz (**GATE Q7**) |
| Fail-safe | stream alive ~ms after power-on (NO_READING); dead board = silence → logger 500 ms staleness rule (**deployment gate Q11**) |
| Baud accuracy | +0.005 % (crystal + fractional divider) vs logger's ±2 % window |

## 7. Resource budget (step-4b build, ARMCLANG V6.24 -O0, debug target)

| Resource | Used | Capacity | % |
|---|---|---|---|
| Flash (Code + RO) | 21,140 + 3,316 = 24,456 B | 65,536 B | **37 %** (IROM1 shrink to 0xF000 pending — still >2× headroom) |
| RAM (RW + ZI) | 20 + 2,144 = 2,164 B | 4,096 B | **53 %** (1 KB console TX ring is the largest single object; production build reclaims most console RAM) |
| NVM data flash | 2 × 128 B pages (settings v4 @ 0xFF00, cal @ 0xFF80) | 4 KB | 6 % — ~30 pages free for the future temperature LUT |
| Build health | 0 errors / 0 warnings | | host gates: 9 C protocol tests + 17 Python decoder tests green |

## 8. Power & environment

| Item | Value |
|---|---|
| Input supply | VS 5.5–28 V (chip rating; board's operating point per harness) |
| Sensor excitation budget | VDDEXT 40 mA limit vs ~1 mA/bridge ×2–3 + mirror overhead — large headroom on paper (**UNVERIFIED**: measure actual mirror current) |
| System power | **40 mW placeholder — UNMEASURED**; continuous ~9.1 pkt/s TX adds ~8 % line-driving duty vs the old DC analog line (bench Test 13 measures reality) |
| **System temperature envelope** | Electronics: −40…+175 °C (T_j) · Sensor survival: −40…+150 °C · **Sensor ACCURACY: −10…+80 °C compensated** — the binding constraint downhole; beyond it, expect thermal drift until the temperature-LUT project lands (die-temp sensors already exist on ADC2) |

## 9. Verified vs. open

**Verified (host/build):** protocol timing invariants (simulated wire, 10 s
runs, stall/fence/abort scenarios), encoder/decoder cross-check (byte-exact
golden stream), Keil build clean, both build variants sweep clean.

**Open — each with its resolving test:**

| Item | Resolves via |
|---|---|
| Front-end gain / counts-per-bar / excitation current | bench Test 9 + a current measurement |
| ADC noise ≥1 LSB (the 12-bit claim) | bench Test 9 RAW capture |
| TI-interrupt timing (stop-bit start vs end) | bench Test 2 TI-offset check |
| Real power draw | bench Test 13 |
| Wire timing on real silicon | bench Test 2 (scope/LA) ★ |
| Logger staleness rule + questionnaire | logger designer (deployment blocker) |
| Harness pull-up on P1.0 | hardware/deployment |
| Sensor behavior beyond +80 °C | temperature-LUT project (future) |
