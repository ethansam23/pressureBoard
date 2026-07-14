# Windows Handoff — Keil Build (step 4b) + Bench Verification

_The digital-link rearchitecture was developed and host-tested on a machine
without Keil. This file is the handoff to the Windows/Keil/J-Link machine:
a paste-ready prompt for the Claude session there, plus the rules it must
work under. Delete this file once the branch is merged._

---

## Paste this prompt into the Windows Claude session

```
Check out the branch claude/rearchitecture and read CLAUDE.md and
WINDOWS_HANDOFF.md before touching anything.

This branch replaces the analog PWM output with a one-way UART packet
stream (see link_protocol.md). All app code is written and host-tested,
but it has NEVER been compiled in Keil — that is your job (step 4b),
followed by bench verification.

Step 1 — Keil project update. The project must lose app/output.c (deleted
on this branch) and gain app/link_frame.c and app/link_tx.c in the app
source group. CLAUDE.md says the .uvprojx is user-managed: ask me to make
those three changes in the uVision IDE (Project tree -> remove output.c;
Add Existing Files -> link_frame.c, link_tx.c). Only edit the .uvprojx XML
yourself if I explicitly tell you to in this session. Do not touch
.uvoptx.

Step 2 — Build:
  & "$env:LOCALAPPDATA\Keil_v5\UV4\UV4.exe" -b dh_pressureboard_attempt1.uvprojx -j0 -o build_fixes.log
Read build_fixes.log. Iterate until 0 errors / 0 warnings, but ONLY under
the fix policy in WINDOWS_HANDOFF.md — never edit RTE/, never change
protocol semantics or timing constants, never silence a warning by
weakening a check. If a fix would violate the policy, stop and ask me.
After any change to app/link_frame.c you must rerun the host gates:
  make -C host_tests    and    cd host_ui && python -m unittest
(both must stay green; if gcc isn't available here, say so and we'll
gate that part back on the other machine).

Step 3 — Commit step 4b on the SAME branch: the .uvprojx change plus any
app/ fixes, message starting "keil: project update for link modules
(step 4b)" and describing every fix made. Push to
origin/claude/rearchitecture.

Step 4 — Bench verification per verification_guide.md, in order. Tests
1-3 (packets-only boot, scope timing, console mutual exclusion) are the
minimum before we call the firmware alive; the full suite is the sign-off.
Record nominal AND worst values for every timing row. Remember: J-Link
DETACHED for all boot/watchdog tests (WDT1 is disabled under debug), and
boot is intentionally silent on the terminal — a board that prints
nothing at boot is CORRECT; watch the LED and the binary stream (the
host_ui monitor decodes it, passive mode, 9600 baud).

Step 5 — Report results against the acceptance criteria in
TLE9854_pressure_transmitter_PRD_rev2.md section 9, listing any failures
with the captured evidence.
```

---

## Fix policy (binding for the Windows session)

**MAY:**
- Fix genuine compile/link errors and warnings **inside `app/` only**:
  missing includes, type mismatches, ARMCLANG-specific diagnostics
  (the code was swept with gcc `-Wall -Wextra` against the SDK headers, but
  ARMCLANG V6.24 at -O0 may flag things gcc did not).
- Run/re-run the host test gates; edit documentation.
- Create an optional second Keil target "production" that defines
  `LINK_CONSOLE_EN=0` (compiles the console out — TX purity).

**MUST NOT:**
- Edit anything under `RTE/` — vendor code, read-only. The rearchitecture
  needed zero RTE changes; a build error pointing there means an app-side
  mistake, not an RTE fix.
- Alter wire-protocol semantics or constants (`link_frame.h`: sync byte,
  code map, `LINK_*` timing values). Payload is sacred; timing values are
  review-derived with margin math. Protocol changes are a renegotiation
  with the logger designer, not a build fix.
- Silence a warning by deleting/weakening a check, cast-hiding a real
  narrowing, or removing a `volatile`.
- Re-add anything from the analog output (output.c, PWM, RANGE), change
  the NVM magics, or touch `.uvoptx`.

**MUST:**
- Keep all commits on `claude/rearchitecture`.
- Rerun both host gates after any `app/link_frame.c` edit (it compiles on
  both toolchains by design — keep it that way).
- Preserve the SDK-free property of `link_frame.{c,h}` (no RTE includes).

## Keil specifics

- Only the source list changes; target settings stay as-is. (The pending
  IROM1 → `0xF000` shrink is a separate, user-owned decision — not part of
  step 4b.)
- Expected outcome: 0 errors / 0 warnings on the debug target. The
  `_Static_assert`s in `acquisition.c` require C11 — ARMCLANG V6 defaults
  cover this; if the project forces an older -std, the asserts are guarded
  and drop out (report it rather than changing the guard).

## Bench prerequisites (from the docs — don't skip)

- Scope or logic analyzer on **P1.0** is mandatory (verification guide
  Test 2 is the core test).
- Terminal/monitor at **9600** 8N1 (not 115200 — changed).
- First boot after flashing wipes settings + calibration **by design**
  (both NVM magics bumped): re-enter settings, re-calibrate before the
  wire is expected to carry pressure.
- Deployment (not bench) additionally gates on the logger-designer
  questionnaire in `link_protocol.md` — staleness rule Q11 above all — and
  the harness idle-high pull-up on P1.0 (Q17).

## Where everything lives

| Topic | File |
|---|---|
| Build cmd, constraints, gotchas | `CLAUDE.md` |
| Wire spec + logger questionnaire | `link_protocol.md` |
| Hardware test procedure | `verification_guide.md` |
| Bench console commands | `uart_command_reference.md` |
| Requirements + acceptance criteria | `TLE9854_pressure_transmitter_PRD_rev2.md` §9 |
| Host gates | `make -C host_tests` · `host_ui: python -m unittest` |
