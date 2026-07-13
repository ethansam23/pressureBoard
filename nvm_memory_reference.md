# TLE9854QXW Memory Map & NVM Reference — Pressure Transmitter

What lives where, how the data flash actually works, and the failure modes
that matter. Sources: TLE9854QXW datasheet Rev 1.2 §9/§10/§29.4, TLE985x
Firmware User Manual Rev 2.0 §5 (verbatim quotes noted), and this firmware.

---

## 1. The map

| Address range | Size | What | Who writes it |
|---|---|---|---|
| `0x0000_0000–0x0000_5FFF` | 24 KB | **BootROM** — startup, FastLIN BSL, the `user_nvm_*` flash routines we call | Infineon (mask ROM, immutable) |
| `0x1100_0000–0x1100_EFFF` | 60 KB | **Code flash** (linear region) — vector table at `0x1100_0000`, our program (~22 KB used) | J-Link / BSL flashing only |
| `0x1100_F000–0x1100_FFFF` | 4 KB | **Data flash** (MAPPED region, "EEPROM emulation") | Firmware at runtime via BootROM calls |
| ├ `0x1100_FF00` | 128 B | **Settings page** — magic `0x53455402`, rate, thresh, probe mode, range window | `RATE/THRESH/RANGE/PROBE` commands |
| └ `0x1100_FF80` | 128 B | **Calibration page** — magic `0xCA11DA7B`, slope, offset, up to 8 points | `CAL STORE` / `CAL CLEAR` |
| `0x1800_0000–0x1800_0FFF` | 4 KB | **SRAM** — stack, statics (~2.1 KB used incl. 1 KB UART TX ring), BootROM scratch during NVM ops | everything |
| (inside NVM, separate) | 512 B | **100TP** — option bytes, 100-write-limited config | nobody (leave alone) |

Page size 128 B (minimum write/erase unit). Sector erase 4 KB. The Keil
project's IROM currently spans the full 64 KB — **the pending action item is
to shrink IROM1 to `0xF000`** so the linker can never place code into the
data-flash sector.

## 2. How the mapped data region really works (it's not plain flash)

The 4 KB data sector is **virtualized**. You address *logical* pages
(`0x1100FF00`, `0x1100FF80`); a RAM table called the **MapRAM** maps each
logical page to whichever *physical* page currently holds its data. Spare
physical pages rotate randomly.

A write to a used page (what every save in this firmware now does):
1. BootROM copies the old page into the NVM's internal assembly buffer,
2. overlays your new data,
3. programs a **spare** physical page (~4 ms),
4. updates the MapRAM to point at the new page,
5. *then* erases the old physical page (~5 ms) and selects a new spare.

Consequences worth knowing:
- **Power-fail safety:** until step 4 commits, the OLD data is still mapped.
  A power cut mid-save loses the new value, never the old one. (This is why
  the firmware no longer pre-erases before writing — the old erase-then-write
  pattern destroyed the stored data *first*.)
- **Built-in wear leveling:** the random spare rotation spreads writes across
  all physical pages of the sector, so endurance is shared.
- **An erased logical page is "unmapped"**, not 0xFF: per the UM, *"only
  mapped pages return data. The read access to an unmapped page causes a
  NMI."* On this build all NMI sources are masked (`SCU_NMICON=0`), so the
  read returns **unspecified data** instead — the 32-bit magic words are the
  guard that makes boot-time loads safe on a virgin/cleared page.
- The BootROM re-derives the MapRAM at every boot by scanning the sector
  (the **Service Algorithm**), repairing single faults; results land in
  `SCU_MEMSTAT` / `SCU_SYS_STRTUP_STS.MRAMINITSTS`.

## 3. The bad things, ranked

| # | Failure | Cause | Symptom | Protection in this firmware |
|---|---|---|---|---|
| 1 | **Core lockup during a flash op** | Any code/exception fetch from the flash macro while it's busy programming (~5–10 ms): the single flash can't be read mid-op, and our ISRs live in it | Hang; standalone the WDT resets ~1 s later, ×5 → Sleep latch | IRQs masked around every BootROM call; never add code that runs from flash during a save (incl. debugger memory windows — see #6) |
| 2 | **Watchdog vs flash timing** | NVM op stretches the loop past WDT1's service point | Reset mid-save | `WDT1_SOW_Service` opens a 30 ms short-open-window before each op (architected for exactly this, datasheet Fig. 18); op is a single ~5–10 ms write |
| 3 | **MapRAM inconsistency** (`MRAMINITSTS` set) | Interrupted erase/write at the wrong instant, double mapping the Service Algorithm can't repair, ECC2 error | Settings/cal unreadable or stale; further writes UNSAFE | Checked once at boot; if set: boot prints `WARN: NVM data flash inconsistent (saves disabled)` and **all saves are refused**. Recovery = full chip erase + reflash (BootROM rebuilds the MapRAM) |
| 4 | **Endurance exhaustion** | 30,000 erase/program cycles per cell (shared across the sector by wear leveling); drain-disturb limit 32,000 | Verify failures (`rc!=0` on save), eventually data loss | Saves happen only on operator command, never periodically — practical usage is orders of magnitude below the limit. Don't ever put a save in the refresh loop |
| 5 | **Code/data collision** | Linker places code into `0x1100F000+`, then a runtime save erases it | Hard fault / corrupted firmware after first save | Currently only ~22 of 60 KB used; **close it for good with the IROM1 = `0xF000` Keil change** |
| 6 | **Debugger interference** | Keil/J-Link polling flash memory (memory window, periodic update) while the macro is busy | The historic CAL STORE freeze (only ever seen J-Link-attached) | Keep memory/watch windows closed + Periodic Window Update off during saves; or test saves standalone |
| 7 | **Retention** | 20 years @ 1000 cycles (datasheet) | Drift decades out | Non-issue for this tool's life; Disturb Handling refreshes stale pages automatically (~1/1000 writes) |
| 8 | **100TP corruption** | Writing the 100-times-programmable config region | Boot behavior changes permanently | We never touch it; don't |

## 4. What each save costs

One settings save (`RATE`/`THRESH`/`RANGE`/`PROBE`) or `CAL STORE` =
**one page-write cycle** (~5–10 ms flash-busy, IRQs masked). `CAL CLEAR` =
one page-erase. The `nvm: … rc=` diagnostic brackets every op; `rc=0` is
success, negative values are BootROM error codes (`bootrom.h`
`BOOTROM_RETURN_CODE_t`).

## 5. RAM notes

4 KB total. Current build: ~2.1 KB statics (dominated by the 1 KB UART TX
ring) + stack. The BootROM NVM routines run on the caller's stack and use
SRAM internally — keep comfortable stack headroom (we have ~1.9 KB). Keep
LTO **off** in the Keil project: the SDK's `WD_Counter` is non-volatile and
cross-module optimization could hoist its read, starving the watchdog.
