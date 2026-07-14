# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a downhole pressure transmitter on the Infineon **TLE9854QXW** (ARM Cortex-M0, Keil µVision project). Two ratiometric pressure probes on ADC1 (12-bit-scaled oversampling), multi-point linear calibration applied **on-board**, and a **one-way digital UART packet stream** (9600 8N1 on P1.0) to the battery/logger — the only downhole interface. A bench-only debug console shares the same UART under strict mutual exclusion (boots LOCKED; `CONSOLE UNLOCK` suspends the stream). Wire spec + logger questionnaire: `link_protocol.md`. There is no emulator — the protocol core has a gcc host test suite (`host_tests/`), everything else is verified on hardware (`verification_guide.md`).

## Build / flash / run

- Firmware build (Keil CLI; IDE build works too):
  ```
  & "$env:LOCALAPPDATA\Keil_v5\UV4\UV4.exe" -b dh_pressureboard_attempt1.uvprojx -j0 -o build_fixes.log
  ```
  Exit code 0 = clean, 1 = warnings; read `build_fixes.log` for the result. Compiler is ARMCLANG V6.24 at -O0. Production variant: define `LINK_CONSOLE_EN=0` in a dedicated Keil target (compiles the console out entirely — TX purity).
- **Host tests (run before any firmware commit touching the link):** `make -C host_tests` (gcc protocol/timing simulation, emits `golden_stream.csv`) and `cd host_ui && python3 -m unittest` (decoder tests, cross-checked against the golden stream). These gate logic only — **they are not a Keil build.**
- **Never edit `dh_pressureboard_attempt1.uvprojx` / `.uvoptx`** — the user manages the Keil project file himself. If a new source file needs adding to the project, tell the user; don't do it.
- Flash/debug is via J-Link from the Keil IDE (user-driven). **WDT1 is disabled under J-Link**, so watchdog bugs only reproduce on a standalone power-cycled boot. Note: with the console locked, boot is intentionally silent ASCII-wise — watch the LED and the binary stream, not for a banner.
- Host GUI: `cd host_ui && pip install -r requirements.txt && python pressure_monitor.py` — **passive packet monitor by default** (never transmits); "Bench Mode" opt-in sends `CONSOLE UNLOCK`.

## Layout

- `app/` — all application code. This is the only place firmware changes go.
- `host_tests/` — gcc harness for `app/link_frame.c` (pure protocol core). Keep `link_frame.{c,h}` SDK-free so this keeps compiling.
- `host_ui/` — monitor GUI + `link_decoder.py` (+ unittest suite).
- `RTE/Device/TLE9854QXW/` — Infineon-generated SDK. Treat as read-only vendor code; reference it for driver contracts, don't modify it. (The link rearchitecture required zero RTE edits — UART2's existing ISR callback wiring is reused.)
- `TLE9854QXW_Programming_Reference.txt`, `tle9854qxw_datasheet.json`, `tle985x_firmware_usermanual.json`, `keller_pressure_sensor.json` — searchable hardware reference material.

## Architecture

Cooperative super-loop in `app/main.c` — no RTOS, no blocking waits. Every module exposes `*_init()` plus a non-blocking service/run function; `main.c` owns all sequencing:

1. `scheduler_service()` — SysTick 1 ms tick; services WDT1 only when its window is open.
2. On refresh (default 1 Hz, settable 100–5000 ms; the pending flag is consume-once so main latches it): **fence first** (`link_tx_fence_bounded()` — acquisition can stall ~34 ms and must never tear a packet; fail → defer + retry, 3-strike escape) → `acquisition_run()` → per-cause supervision (`fault_raise_adc/vddext`) → `fault_check()` (disagreement, dual-probe only) → `calibration_service()` (paused during faults) → **link code priority ladder** (`ADC_STALL > VDDEXT > DISAGREE > UNCAL > link_encode_bar(pressure)`) → `link_tx_set_live_code()` → release → `uart_cmd_update_readings()`.
3. Background every iteration: `link_tx_service()` **first** (owns the wire deadline), `led_arbitrate()` (main.c is the single owner of LED state), `status_led_service()`, `uart_cmd_service()`.

Link engine split: `link_frame.{c,h}` = pure protocol (packet SM, encoder, checksum — host-tested, keep SDK-free); `link_tx.{c,h}` = UART2 shim, single TX-owner arbiter (packet mode vs bench-console mode — never interleaved), fail-closed fence, LINKTEST. The stream boots within ms carrying `NO_READING` (fail-safe: the wire never shows fake pressure; a dead board is silence — the logger's staleness timeout covers that, see `link_protocol.md`).

All pins, tuning constants, addresses, and defaults live in `app/app_config.h` — change constants there, not inline. Wire-protocol constants live with the protocol in `link_frame.h`.

## Hard constraints

- **WDT1**: `SystemInit()` (startup) already issued the first WDT1 trigger. Never call `WDT1_Init()` or add extra WDT1 triggers — a second service inside the closed window causes a reset, and 5 consecutive watchdog resets latch the chip into Sleep Mode. All servicing goes through `scheduler_service()` (the fence loops service it explicitly). Long operations must stay well under the ~300 ms budget.
- **Never spin unbounded on hardware status bits** — time-box and fall back to a fault. Downhole, the packet stream is the only signal; the fault-cause codes are the failure path.
- **The link fence is mandatory** before anything that stalls the CPU or masks IRQs for milliseconds (every `user_nvm_*` call, blocking acquisition bursts). Fail-closed: skip/defer + report, never stall over a live packet. `link_tx_release()` transmits nothing (overdue policy: one packet, rebased, no catch-up burst).
- **TX purity**: nothing but packets on the wire outside an unlocked bench session. No `uart_send_*` at boot or in packet mode (they're no-ops then, but don't add dependencies on them); production compiles the console out.
- **Payload is sacred**: never escape/nudge/alter wire values; 0x7F is legal in payload and checksum. Protocol changes are a renegotiation with the logger designer, not a code edit.
- **NVM data flash** (`0x1100F000`, 4 KB, MapRAM — see `nvm_memory_reference.md`): BootROM `user_nvm_*`, ~5–10 ms IRQ-masked stalls, health-gated (`nvm_flash_is_healthy()`), fence-gated. Settings page `0x1100FF00` (magic v4), calibration page `0x1100FF80`.
- **Units**: firmware is bar-native; wire is deci-bar absolute (0–1000.0 bar fixed); PSI exists only at the console boundary.
- `float` is used freely in the main loop (calibration, encode — softfloat on M0, fine at 1 Hz); **never in ISRs**. IEEE-754 single, no fast-math (host-test vectors must transfer to ARMCLANG).

## UART / console protocol

`app/uart_cmd.c` (`process_cmd`) is the source of truth; **update `uart_command_reference.md` whenever commands change**. 9600 8N1, shared with the stream, case-insensitive, line-based, boots locked. TX is a 1 KB ring drained only in console mode (~960 B/s). Removed vs the analog firmware: `OUTPUT`, `RANGE`. Added: `CONSOLE UNLOCK/LOCK`, `LINKTEST`.

## Current state / gotchas

- **Flashing this firmware wipes settings + calibration by design** (both NVM magics bumped for the 12-bit count scale) — re-enter settings, re-calibrate on a rig before the wire carries pressure.
- Counts are 12-bit-SCALED (0–4092, `sum/4`); "effective bits" is unproven until the bench RAW noise capture (verification guide Test 9). RAW/SCAN diagnostics stay native 10-bit (4× factor vs production counts).
- TI timing is a **working hypothesis** (stop-bit start; +1-bit allowance everywhere) pending a scope measurement — don't tighten timing constants without that data.
- P1.0 is high-Z during MCU reset — the harness pull-up (logger side) is a deployment requirement, not firmware's job.
- TEMP `nvm: … rc=` prints are slated for removal — don't build on them.
- Deployment gates: logger questionnaire in `link_protocol.md` (staleness Q11 above all); Keil production target (`LINK_CONSOLE_EN=0`); pending IROM1 shrink to `0xF000` (project-file change — user's to make).

## Reference docs

- `link_protocol.md` — **normative wire spec** + logger-designer questionnaire + rate/gap metrics.
- `TLE9854_pressure_transmitter_PRD_rev2.md` — requirements, acceptance criteria (Rev 1 = analog era, superseded, history only).
- `uart_command_reference.md` — bench console surface.
- `nvm_memory_reference.md` — memory map and how the mapped data flash actually behaves.
- `verification_guide.md` — on-hardware test procedure (standalone, J-Link detached; scope on P1.0 is mandatory).
