# Post-Fix Bench Checklist — updated 10 Jun 2026 (round 2)

Round-1 status: **A (standalone power-cycle) PASSED**, **D (NVM saves
standalone) PASSED** — CAL STORE and RATE saves work with `rc=0`. The
historic freezes were J-Link-attached (keep memory/watch windows closed and
Periodic Window Update off when debugging saves).

This round covers the remaining items plus everything round 2 changed
(double window-count fix, ADC-stall fault, PSI front end, parser hardening).
Full procedures: `verification_guide.md`. Memory background:
`nvm_memory_reference.md`.

Build: 0 errors / 0 warnings; boot diagnostics (`RST …`, `t=` markers,
`nvm: … rc=`) are TEMP and will be stripped after this checklist passes.

---

## 1. Boot & fail-safe re-check (5 min, J-Link detached)

1. [ ] Power-cycle: ONE banner; `RST 0x…` line shows `POR`/`PIN`, **not WDT1**
2. [ ] Markers print in order: `vddext` → `loop` → `wdt-svc t≈700` →
       `refresh t≈1000`, then `wdt-svc` ~every 700 ms (two more prints)
3. [ ] RST button → fresh boot, cause line shows `PIN`
4. [ ] P0.1 sits at ~0.25 V until the first reading (~1 s), then live
5. [ ] 10-min soak with `AUTO` on — no re-banner, no gaps

## 2. PSI front end (new)

6. [ ] `PSI 14.7` → `14.700 psi = 1.013 bar`
7. [ ] `BAR 1` → `1.000 bar = 14.503 psi`
8. [ ] `CAL ARM` then `CAL 14.7 PSI` → `Capturing at 1.013 bar (14.699 psi)...`
9. [ ] `RANGE 0 8700 PSI` → rejected (`>1000 bar`); `RANGE 0 145 PSI` →
       `Range=0.000-9.997 bar (saved)`; restore: `RANGE 1 1000`
10. [ ] `CAL ABORT` to leave the session

## 3. Parser hardening (new)

11. [ ] `RATE 1000x` → `ERR: rate 100-5000` (was silently accepted)
12. [ ] `OUTPUT 10O` → `ERR: OUTPUT <0-1023>|AUTO`
13. [ ] `RANGE 0  600` (double space) still works; `RANGE x 600` → ERR
14. [ ] Bare `RATE` / `CAL` / `OUTPUT` → usage string, not "unknown"
15. [ ] `STATUS ` (trailing space) works
16. [ ] Paste 3 commands in one go → all three answered, in order, complete

## 4. Calibration UX (new behaviors)

17. [ ] `RATE 100`, `CAL ARM`, `CAL 14.7 PSI` → `Captured (1 pts)` in ~0.8 s
18. [ ] During a capture: `CAL STORE` → `ERR: capture in progress…`
19. [ ] Two captures at the same pressure/counts → `CAL STORE` →
        `ERR: degenerate fit (points at same counts)`
20. [ ] Real 2-pt cal → `CAL STORE` → `nvm: cal write... rc=0` then
        `Cal stored: slope=… offset=…`; LED solid 2 s
21. [ ] Power cycle → `CAL STATUS` shows `VALID` with the same `pts=` count
22. [ ] `CAL CLEAR` → `nvm: cal erase... rc=0`, `Cal cleared`
23. [ ] `RATE 1000` restore

## 5. Fault behavior (new sources + hysteresis)

24. [ ] `THRESH 1` with probes apart → `Fault: YES`, output ~0.25 V,
        double-blink LED; `THRESH 200` → recovers, LED back to previous state
25. [ ] Hover test (optional): a diff right at the threshold no longer flaps
        the output every refresh (~thresh/8 hysteresis)
26. [ ] `OUTPUT 512` during an active fault → `(latched; fault active...)`,
        line stays 0.25 V; clear fault → line goes to ~2.5 V; `OUTPUT AUTO`
27. [ ] During a fault, an in-progress capture pauses (no `Captured` until
        the fault clears)
28. [ ] STATUS `Fault:` always matches the LED

## 6. Remaining from round 1

29. [ ] LED pattern walk: heartbeat → armed (1 Hz) → capturing (5 Hz) →
        stored (solid 2 s) → heartbeat; fault double-blink wins over cal,
        returns to the cal pattern when cleared
30. [ ] `RAW`/`SCAN` sane, `valid=16/16`; `AUTO` tags correct, `Out:` matches
        the meter on the same line (now same-cycle)
31. [ ] **Keil (your side): IROM1 size → `0xF000`**, rebuild, still clean
        (see `nvm_memory_reference.md` §3 item 5 for why)

## 7. When all boxes tick

- [ ] Tell Claude to strip the TEMP diagnostics (reset-cause line, `t=`
      markers, `nvm: rc=` prints, `uart_tx_flush_bounded`) and rebuild
- [ ] Re-run items 1, 20, 24 once on the clean build as a smoke test

---

## Parked design decisions (unchanged, need human/team input)

- THRESH-in-counts vs Keller probe-to-probe tolerances (per-probe cal or
  matched probes)
- ±1–2 bar acceptance vs sensor's ±2.5 bar floor + temp coefficients
- Current-mirror voltage compliance at temperature (scope it)
- POWER characterization (measure mW per RATE; table goes in then)

---

## SUPERSEDED (14 Jul 2026 — digital-link rearchitecture)

The analog-output items above (P0.1 voltages, OUTPUT command, fault-band
levels, RANGE) are obsolete: the PWM-DAC was replaced by the one-way UART
packet stream on P1.0 (see `link_protocol.md`). Open items now tracked in
`TLE9854_pressure_transmitter_PRD_rev2.md` §10 and the rewritten
`verification_guide.md`. Still-valid learnings carried forward: J-Link-
attached NVM freezes (keep memory windows closed), the WDT1 double-count
history, and the standalone power-cycle discipline.
