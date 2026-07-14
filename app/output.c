#include "output.h"
#include "app_config.h"
#include "nvm_config.h"
#include "ccu6.h"
#include "port.h"

/* ---- Compile-time duty-cycle constants (no runtime float) ---------------- */
#define DUTY_MAX        (PWM_MAX_COUNT - 1u)                                     /* 1023 */
#define DUTY_LO         ((uint16)(OUT_V_LO   / OUT_V_SUPPLY * (float)DUTY_MAX + 0.5f)) /* 102 */
#define DUTY_HI         ((uint16)(OUT_V_HI   / OUT_V_SUPPLY * (float)DUTY_MAX + 0.5f)) /* 921 */
#define DUTY_FAULT_LO   ((uint16)(FAULT_V_LO / OUT_V_SUPPLY * (float)DUTY_MAX + 0.5f)) /* 51  */
#define DUTY_FAULT_HI   ((uint16)(FAULT_V_HI / OUT_V_SUPPLY * (float)DUTY_MAX + 0.5f)) /* 972 */
#define DUTY_SPAN       (DUTY_HI - DUTY_LO)                                     /* 819 */

/* ---- State --------------------------------------------------------------- */
static bool   manual_override;
static uint16 manual_duty;              /* duty pinned by OUTPUT <n>          */
static uint16 cur_duty;                 /* last value handed to the PWM       */

/* ---- Forward declarations ------------------------------------------------ */
static void set_duty(uint16 duty);

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */
void output_init(void)
{
    manual_override = false;

    /* P0.1 → push-pull output, alt-function 2 = CCU6 CC62_0 */
    PORT_P01_Output_Set();
    PORT_ChangePinAlt(0x01u, 2u);

    /* CCU6 T12: fCCU6/2 = 20 MHz, period 1023, edge-aligned → ~19.5 kHz    */
    CCU6_T12_Clk_Sel(1u);              /* fCCU6 / 2                         */
    CCU6_T12_Prescaler_Dis();           /* no additional prescaler           */
    CCU6_T12_Edge_Aligned_Mode_En();    /* count-up, wrap at period          */
    CCU6_T12_Period_Value_Set(DUTY_MAX);/* 10-bit range: 0-1023              */

    /* Channel 2: compare mode with output toggle enabled                    */
    CCU6_Ch2_CapCom_Mode_Sel(1u);       /* MSEL62 = 1 → compare output on   */

    /* Boot fail-safe (PRD §9 step 6): hold the line in the OUT-OF-BAND
     * fault-low region until the first real reading lands. DUTY_LO (0.5 V)
     * would read as a legitimate "pressure = range_lo" to the battery while
     * the tool may be at depth. (Routed via set_duty for the active-low
     * CC62 polarity.)                                                       */
    set_duty(DUTY_FAULT_LO);

    /* Enable CC62 modulation output (bit 4 of T12MODEN)                     */
    CCU6_T12_Modulation_En(0x10u);

    /* Load shadow registers and start timer                                 */
    CCU6_EnableST_T12();
    CCU6_StartTmr_T12();
}

void output_set_pressure(uint16 counts)
{
    uint32 duty;
    if (manual_override) { set_duty(manual_duty); return; }   /* re-assert after a fault episode */
    /* Counts are 12-bit-scaled (0-4092) as of the oversample change; keep
     * this doomed analog path correct until the link output replaces it.    */
    if (counts > ADC_COUNTS_MAX) { counts = ADC_COUNTS_MAX; }
    duty = (uint32)DUTY_LO + ((uint32)counts * (uint32)DUTY_SPAN) / ADC_COUNTS_MAX;
    set_duty((uint16)duty);
}

void output_set_pressure_bar(float bar)
{
    float lo = nvm_config_get_range_lo_bar();
    float hi = nvm_config_get_range_hi_bar();
    float frac;
    uint16 duty;
    if (manual_override) { set_duty(manual_duty); return; }   /* re-assert after a fault episode */

    if (bar <= lo)      { frac = 0.0f; }
    else if (bar >= hi) { frac = 1.0f; }
    else                { frac = (bar - lo) / (hi - lo); }

    duty = (uint16)((float)DUTY_LO + frac * (float)DUTY_SPAN + 0.5f);
    set_duty(duty);
}

void output_set_fault_low(void)
{
    set_duty(DUTY_FAULT_LO);
}

void output_set_fault_high(void)
{
    set_duty(DUTY_FAULT_HI);
}

void output_set_manual(uint16 counts)
{
    uint32 duty;
    manual_override = true;
    if (counts > 1023u) { counts = 1023u; }
    duty = (uint32)DUTY_LO + ((uint32)counts * (uint32)DUTY_SPAN) / 1023u;
    manual_duty = (uint16)duty;
    set_duty((uint16)duty);
}

void output_set_auto(void)
{
    manual_override = false;
}

/* ========================================================================= */
/*  Internal helpers                                                         */
/* ========================================================================= */
static void set_duty(uint16 duty)
{
    if (duty > DUTY_MAX) { duty = DUTY_MAX; }
    cur_duty = duty;                 /* keep INTENDED duty for STATUS/AUTO    */
    /* CC62 is active-low on this board: a raw compare write inverts the analog
     * output (measured: OUTPUT 0 -> 4.5 V, 1023 -> 0.5 V). Write the COMPLEMENT
     * so higher duty = higher voltage, and the fault bands land correctly.   */
    CCU6_Ch2_Value_Set((uint16)(DUTY_MAX - duty));
    CCU6_EnableST_T12();
}

uint16 output_get_duty(void)  { return cur_duty; }
bool   output_is_manual(void) { return manual_override; }
