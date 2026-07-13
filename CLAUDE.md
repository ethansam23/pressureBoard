# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a downhole pressure transmitter on the Infineon **TLE9854QXW** (ARM Cortex-M0, Keil µVision project). Two ratiometric pressure probes on ADC1, multi-point linear calibration, PWM-DAC analog output (0.5–4.5 V, fault bands outside), and a bench-only UART command interface. There is no test suite and no emulator — verification is on hardware (see `verification_guide.md`).

## Build / flash / run

- Build (Keil CLI; IDE build works too):
  ```
  & "$env:LOCALAPPDATA\Keil_v5\UV4\UV4.exe" -b dh_pressureboard_attempt1.uvprojx -j0 -o build_fixes.log
  ```
  Exit code 0 = clean, 1 = warnings; read `build_fixes.log` for the result. Compiler is ARMCLANG V6.24 at -O0.
- **Never edit `dh_pressureboard_attempt1.uvprojx` / `.uvoptx`** — the user manages the Keil project file himself. If a new source file needs adding to the project, tell the user; don't do it.
- Flash/debug is via J-Link from the Keil IDE (user-driven). **WDT1 is disabled under J-Link**, so watchdog bugs only reproduce on a standalone power-cycled boot — a repeating boot banner that goes dead after ~5 resets means the chip latched into Sleep Mode from watchdog resets.
- Host GUI: `cd host_ui && pip install -r requirements.txt && python pressure_monitor.py` (Tkinter dashboard speaking the same UART protocol).

## Layout

- `app/` — all application code. This is the only place firmware changes go.
- `RTE/Device/TLE9854QXW/` — Infineon-generated SDK (peripheral drivers, `tle_device.h`). Treat as read-only vendor code; reference it for driver contracts, don't modify it.
- `TLE9854QXW_Programming_Reference.txt`, `tle9854qxw_datasheet.json`, `tle985x_firmware_usermanual.json`, `keller_pressure_sensor.json` — searchable hardware reference material.

## Architecture

Cooperative super-loop in `app/main.c` — no RTOS, no blocking waits. Every module exposes `*_init()` plus a non-blocking service/run function; `main.c` owns all sequencing:

1. `scheduler_service()` — SysTick 1 ms tick; services WDT1 only when its window is open.
2. On `scheduler_refresh_pending()` (default 1 Hz, UART-settable 100–5000 ms): `acquisition_run()` → VDDEXT/stall supervision → `fault_check()` (probe disagreement, dual-probe mode only) → `calibration_service()` (paused while a fault is active) → output stage → `uart_cmd_update_readings()` (AUTO stream last, so it reflects this cycle's state).
3. Background every iteration: `led_arbitrate()` (main.c is the *single owner* of LED state — modules must not set the LED directly), `status_led_service()`, `uart_cmd_service()`.

Output priority: fault → `output_set_fault_low()`; valid calibration → bar-mapped output; otherwise raw-counts mapping. The output boots in the fault-low band (fail-safe) until the first reading lands.

All pins, tuning constants, addresses, and defaults live in `app/app_config.h` — change constants there, not inline.

## Hard constraints

- **WDT1**: `SystemInit()` (startup) already issued the first WDT1 trigger. Never call `WDT1_Init()` or add extra WDT1 triggers — a second service inside the closed window causes a reset, and 5 consecutive watchdog resets latch the chip into Sleep Mode. All servicing goes through `scheduler_service()`. Long operations must stay well under the ~300 ms service budget (see `ADC_EOC_TIMEOUT_SPINS` comment for the accounting style).
- **Never spin unbounded on hardware status bits** (VDDEXT stable, ADC EOC, NVM busy). Time-box the wait and fall back to a fault — UART and LED are bench-only; downhole, the analog line is the only signal, so out-of-band fault-low is the failure path.
- **NVM data flash** (`0x1100F000`, 4 KB, virtualized/MapRAM — see `nvm_memory_reference.md`): writes go through BootROM `user_nvm_*` calls; a save takes ~ms and CPU stalls matter. Settings page `0x1100FF00`, calibration page `0x1100FF80`, 128-byte pages. NVM saves are gated by `nvm_flash_is_healthy()`.
- **Units**: firmware is bar-native everywhere. PSI exists only at the UART boundary (`PSI` suffix / converter commands).
- `float` is used freely (calibration, output mapping) — it's software FP on M0; fine at 1 Hz, don't put it in ISRs.

## UART protocol

`app/uart_cmd.c` (`process_cmd`) is the source of truth; **update `uart_command_reference.md` whenever commands change**. 115200 8N1 on TX P1.0 / RX P1.1, case-insensitive, line-based. TX is a 1 KB ISR ring buffer that silently drops bytes when full — HELP output (~800 B) is the sizing case for single bursts.

## Current state / gotchas

- TEMP bring-up diagnostics (reset-cause print, `diag_mark` timestamps in `main.c`, `nvm: ... rc=` prints) are slated for removal once commissioning is done — don't build on them.
- Open items and known findings are tracked in `postfix_checklist_2026-06-09.md`; pending hardware TODOs are marked in `app_config.h`.
- Pending: shrink Keil IROM1 to `0xF000` so the linker can't place code in the data-flash sector (project-file change — user's to make).

## Reference docs

- `TLE9854_pressure_transmitter_PRD_rev1.md` — requirements, module breakdown, acceptance criteria.
- `TLE9854_pressure_transmitter_design_notes (1).md` — decided architecture + open decisions.
- `nvm_memory_reference.md` — memory map and how the mapped data flash actually behaves.
- `verification_guide.md` — on-hardware test procedure (run standalone, J-Link detached, for watchdog coverage).
