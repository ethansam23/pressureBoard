export const meta = {
  name: 'fix-batch-verify',
  description: 'Adversarially verify the 9-Jun fix batch in app/ against SDK contracts and hunt regressions',
  phases: [
    { title: 'Review', detail: '4 specialized reviewers over the changed firmware' },
    { title: 'Adjudicate', detail: 'verify each finding' },
  ],
}

const ROOT = 'C:\\Users\\Ethan Sam\\Documents\\Work\\DHToolingProj\\attempt1'

const CONTEXT = `
Project: bare-metal TLE9854QXW pressure transmitter at "${ROOT}". App code in "${ROOT}\\app\\" (small files — read fully). SDK/DFP in "${ROOT}\\RTE\\Device\\TLE9854QXW\\" (treat as correct; Grep, don't read huge headers fully). The project just received a fix batch (9 Jun 2026). It compiles clean (ARMCLANG, 0 errors 0 warnings). The fixes were:
1. main.c: removed WDT1_Init()/SysTick_Init() (SystemInit in system_tle985x.c already calls both before main via Reset_Handler); scheduler_tick() in scheduler.c now calls WDT1_Window_Count() so WDT1_Service() can actually trigger after WD_Counter > 699.
2. output.c: output_init boots at DUTY_FAULT_LO (0.25 V) instead of DUTY_LO; manual override duty stored in manual_duty and re-asserted by output_set_pressure/_bar when manual_override is set.
3. acquisition.c: bounded EOC waits (ADC_EOC_TIMEOUT_SPINS=10000) with last_good[] per-channel fallback; ADC1_GetChResult valid-flag now honored (fallback to last_good); one throwaway conversion added at top of oversample() for mux settle.
4. main.c: VDDEXT settle timeout now calls fault_raise_system(); per-refresh VDDEXT supervision reads VDDEXT_CTRL.STABLE via u1_Field_Rd32 and raises/clears the system fault; fault.c/h gained sys_fault + fault_raise_system/fault_clear_system; fault_is_active() ORs both.
5. LED arbitration centralized in main.c led_arbitrate() (fault > capturing > armed > heartbeat; transient LED_STATE_CAL_STORED allowed to finish); fault.c and calibration.c no longer set the LED except calibration_store pushing CAL_STORED; status_led.c/h gained status_led_get_state().
6. main.c output stage: 'else if (calibration_is_valid())' — dropped the CAL_IDLE condition so a valid cal keeps driving during a session.
7. nvm_config.c/h: boot check of SCU->SYS_STRTUP_STS.MRAMINITSTS → nvm_flash_is_healthy(); nvm_config_save(), calibration save_to_nvm() and calibration_clear()'s erase are gated on it; main prints a boot WARN when unhealthy.
8. calibration.c/h: calibration_store returns cal_store_result_t (OK/TOO_FEW/BAD_FIT/NVM_FAIL); load_from_nvm restores num_pts + pts[]; uart_cmd prints distinct errors.
9. uart_cmd.c: cmd_prefix() case-insensitive prefix matching replaced the memcmp("X ")||memcmp("x ") pairs; parse_u16 saturates at 65535; RATE validates 100-5000 BEFORE persisting (ERR otherwise); OUTPUT rejects non-numeric args; CAL <bar> at the 8-point cap prints an error instead of silently dropping; HELP includes CAL ABORT; <string.h> include removed.
10. app_config.h: UART_TX_BUF_SIZE 256→1024; ADC_EOC_TIMEOUT_SPINS added; PWM_FREQ_HZ corrected to 19531 documented as informational; UART2 comment fixed.
Your job: try to BREAK this batch. Report only real, evidenced problems with file:line. Final output is consumed by a program — use the structured output.`

const FINDINGS = {
  type: 'object',
  required: ['findings'],
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object',
        required: ['title', 'severity', 'code_ref', 'description', 'evidence'],
        properties: {
          title: { type: 'string' },
          severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low'] },
          code_ref: { type: 'string' },
          description: { type: 'string' },
          evidence: { type: 'string' },
        },
      },
    },
  },
}

const VERDICT = {
  type: 'object',
  required: ['isReal', 'adjusted_severity', 'explanation'],
  properties: {
    isReal: { type: 'boolean' },
    adjusted_severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low', 'not-a-problem'] },
    explanation: { type: 'string' },
  },
}

const REVIEWERS = [
  {
    key: 'wdt1-boot-chain',
    prompt: `${CONTEXT}
FOCUS: the watchdog/boot chain. Trace the FULL standalone boot sequence: startup_tle985x.S Reset_Handler -> SystemInit (system_tle985x.c) -> __main -> main.c. Verify: exactly ONE WDT1_TRIG trigger happens before the windowed-service regime begins (SystemInit's WDT1_Init); nothing in TLE_Init() (tle_device.c — read it and the *_Init functions it calls) or the app writes WDT1_TRIG early; scheduler_tick() -> WDT1_Window_Count() advances WD_Counter from the SysTick ISR and WDT1_Service() in scheduler_service() triggers only after WD_Counter > SCUPM_WDT1_TRIGGER (699) — check that the first service lands inside the open window (period SCUPM_WDT1_PERIOD=1008 ms, service threshold 699 ms = ~70%) and that the main loop can never delay a service past the period (estimate the worst-case loop pass: an acquisition_run is 2 channels x 17 conversions, RAW/SCAN debug commands run up to 5 channels x 17 conversions, NVM save ~10 ms with IRQs masked — total must stay well under 1008-699=309 ms... actually under the full period minus the trigger point; do the arithmetic). CRITICAL SUB-CHECK: WDT1_SOW_Service() (wdt1.c:110-115) does a READ-MODIFY-WRITE of the SCUPM->WDT1_TRIG register (Field_Wrt32 of the SOWCONF field). Check the WDT1_TRIG register layout in tle985x.h (SCUPM_WDT1_TRIG_* defines) and determine whether that RMW can itself count as a watchdog service/trigger in the closed window (it is called immediately before NVM saves at arbitrary times in nvm_config.c:89/96 and calibration.c). Also check: WD_Counter is non-volatile uint32 incremented in ISR context and read in thread context via the non-inlined WDT1_Service() — any real tearing/caching risk on Cortex-M0 with ARMCLANG across translation units? Also: scheduler_init() no longer has WDT1_Init's WD_Counter=0 reset in main — confirm WD_Counter starts at 0 anyway (SystemInit's WDT1_Init reset it) and that the elapsed time from SystemInit to the super-loop (TLE_Init + module inits + bounded 100 ms VDDEXT wait) cannot let WD_Counter pass the full period before the first scheduler_service().`,
  },
  {
    key: 'signal-fault-led',
    prompt: `${CONTEXT}
FOCUS: acquisition/output/fault/LED semantics after the batch. Read app/acquisition.c, output.c, fault.c/h, status_led.c/h, calibration.c, main.c fully. Hunt for: (a) last_good[] indexing bugs (channels used: 6,7,8,9,12; array 16; mask 0x0F) and first-call behavior (all-zero last_good before any good sample — what does a boot-time ADC stall output?); (b) the throwaway conversion in oversample() — any timing/feed interaction with calibration_service's CAL_CAPTURE_SAMPLES accounting or the refresh budget at RATE 100 (2 channels x 17 conversions per refresh, each conversion a few µs + 10000-spin worst-case guard — can a pathological ADC make one refresh pass exceed the watchdog service window?); (c) VDDEXT supervision: PMU_VDDEXT_On() in the boot wait ENABLES the regulator; the per-refresh vddext_stable() in main.c only READS VDDEXT_CTRL.STABLE — confirm from the SDK headers (pmu.h, tle985x.h PMU_VDDEXT_CTRL fields) that STABLE stays meaningful in steady state and that ENABLE isn't cleared by a fault latch such that STABLE alone is the wrong thing to poll (would the fault auto-recover require re-calling PMU_VDDEXT_On to re-enable? If VDDEXT latches OFF on overcurrent, does STABLE go 0 and stay 0 — fault latched forever with no re-enable attempt — and is that acceptable/fail-safe or a missed recovery?); (d) LED arbiter: walk every state transition (fault during cal armed, fault during CAL_STORED 2s window, abort during capture, VDDEXT fault at boot before first refresh, single-probe mode) — find any state where the LED sticks wrong or flickers between states each loop pass; (e) output manual re-assert: any path where manual_duty is used before output_set_manual ever ran (boot default 0 -> 0.5V? check static init) — e.g. STATUS/AUTO display vs actual; (f) fault.c rewrite: fault_check no longer edge-triggered — confirm no caller relied on the old transition behavior.`,
  },
  {
    key: 'uart-parser',
    prompt: `${CONTEXT}
FOCUS: uart_cmd.c after the parser changes. Read it fully. For EVERY command, walk the parse path with adversarial inputs: "RATE 100"/"RATE 5000"/"RATE 99"/"RATE 5001"/"RATE"/"RATE x"/"Rate 500"/"RATE  500" (double space)/"RATE 4294967396"; "THRESH 0"/"THRESH 1024"/"THRESH 65536"/"THRESH abc"; "OUTPUT AUTO"/"output auto"/"OUTPUT 1023"/"OUTPUT 1024"/"OUTPUT -1"/"OUTPUT 5x"/"OUTPUT"; "RANGE 0 600"/"RANGE 600 0"/"RANGE 0.5 0.7" (span<1)/"RANGE -5 600"/"RANGE 0 1001"/"RANGE 5"/"RANGE 1e3 2e3"; "PROBE A"/"PROBE a"/"PROBE X"/"PROBE  A"; "CAL ARM"/"cal arm"/"CAL 0"/"CAL -5"/"CAL 1.5.2"/"CAL 1000000"/"CAL STORE" in every state/"CALX"/"CAL"; HELP/STATUS/RAW/SCAN/AUTO casing variants; empty line; 79-char line; backspace past start. Verify each lands in the intended branch with the intended message, and that cmd_prefix can't false-match (e.g. "RANGEX 1 2", "CALIBRATE 5", "OUTPUTX"). Check parse_u16 saturation interacts correctly with each range check, and parse_float on garbage ("CAL abc" -> 0.0 -> which error?). Also verify the TX-side arithmetic: total HELP byte count vs UART_TX_BUF_SIZE=1024 (count the actual string literals INCLUDING the em-dash '—' which is 3 UTF-8 bytes in the source — does the compiled output fit, and is print_scan + its 5x17 conversions still under the buffer?). Check uart_putc's full-buffer drop is still the only overflow behavior and the tx ring indices are consistent with the new size (uint16 head/tail, modulo arithmetic).`,
  },
  {
    key: 'regression-sweep',
    prompt: `${CONTEXT}
FOCUS: fresh-eyes regression sweep of the WHOLE app/ directory plus the two docs. Read every file in app/ end to end (they are small) as if reviewing a PR with no diff: hunt for anything inconsistent, broken, or left half-migrated by the batch. Specifically: stale comments contradicting new behavior; calibration.c — calibration_init/load_from_nvm restore interplay (CAL ARM after reboot resets num_pts? CAL STATUS counts? store->load round-trip layout match between pts[] and cal_nvm_t including padding); nvm_config.c — MRAMINITSTS polarity (set = inconsistent? verify against the SDK header comment and the firmware UM JSON quote: 'If the bit is clear then the data flash mapping is consistent'); unused includes/variables/functions after the LED decoupling (status_led.h includes, fault.c includes nvm_config.h — still needed?); header/impl signature mismatches; main.c — anything that should run but no longer does (the old code called status_led_set_state(LED_STATE_FAULT) on VDDEXT timeout — confirm the arbiter path actually shows FAULT before the first refresh, given led_arbitrate runs every loop pass and fault_raise_system was called); docs "${ROOT}\\uart_command_reference.md" and "${ROOT}\\verification_guide.md" — verify EVERY claim they now make matches the post-fix code exactly (error strings byte-for-byte, HELP line count, voltages, behaviors). Report each discrepancy.`,
  },
]

phase('Review')
log('4 reviewers attacking the fix batch')

const results = await pipeline(
  REVIEWERS,
  (r) => agent(r.prompt, { label: `review:${r.key}`, phase: 'Review', schema: FINDINGS }),
  (found, r) => {
    if (!found || !found.findings || !found.findings.length) return []
    log(`review:${r.key} -> ${found.findings.length} findings`)
    return parallel(
      found.findings.map((f) => () =>
        agent(
          `${CONTEXT}
You are an ADVERSARIAL VERIFIER. A reviewer (${r.key}) claims the problem below. Default stance: SKEPTICAL — re-read the cited files yourself and try to REFUTE it. Only isReal=true if it holds in the current files. Downgrade overblown severity, upgrade understated.

CLAIM:
title: ${f.title}
severity: ${f.severity}
code_ref: ${f.code_ref}
description: ${f.description}
evidence: ${f.evidence}`,
          { label: `verify:${f.title.slice(0, 40)}`, phase: 'Adjudicate', schema: VERDICT }
        ).then((v) => ({ ...f, reviewer: r.key, verdict: v }))
      )
    )
  }
)

const all = results.filter(Boolean).flat().filter(Boolean)
const confirmed = all.filter((f) => f.verdict && f.verdict.isReal)
log(`${all.length} raised, ${confirmed.length} confirmed`)

return {
  confirmed: confirmed.map((f) => ({
    reviewer: f.reviewer,
    title: f.title,
    severity: f.verdict.adjusted_severity,
    code_ref: f.code_ref,
    description: f.description,
    verifier_note: f.verdict.explanation,
  })),
  refuted: all.filter((f) => f.verdict && !f.verdict.isReal).map((f) => ({ reviewer: f.reviewer, title: f.title, refutation: f.verdict.explanation })),
}