#include "calibration.h"
#include "status_led.h"
#include "nvm_config.h"
#include "uart_cmd.h"      /* TEMP: nvm diagnostics (rc prints) — remove with them */
#include "bootrom.h"
#include "wdt1.h"

/* LED note: armed/capturing/fault/heartbeat indication is arbitrated
 * centrally in main from the cal/fault state each loop pass; this module
 * only pushes the transient CAL_STORED pattern (solid 2 s).                 */

/* ---- NVM layout (must fit in one 128-byte page) ------------------------- */
#define CAL_MAGIC  0xCA11DA7Bu   /* bumped: cal points/coeffs now in bar, not psi */

typedef struct {
    uint32_t magic;
    float    slope;
    float    offset;
    uint8_t  num_points;
    uint8_t  reserved[3];
    struct {
        float    pressure_bar;
        uint16_t counts;
        uint16_t pad;
    } points[CAL_MAX_POINTS];
} cal_nvm_t;

/* ---- RAM state ---------------------------------------------------------- */
static cal_state_t state;
static bool        valid;
static float       cal_slope;
static float       cal_offset;

/* Capture accumulator */
static float    cap_pressure;
static uint32_t cap_sum;
static uint16_t cap_count;

/* Point buffer */
static struct {
    float    pressure_bar;
    uint16_t counts;
} pts[CAL_MAX_POINTS];
static uint8_t num_pts;

/* ---- Forward declarations ----------------------------------------------- */
static bool load_from_nvm(void);
static bool save_to_nvm(void);
static bool compute_fit(void);

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */
void calibration_init(void)
{
    state   = CAL_IDLE;
    num_pts = 0u;
    valid   = load_from_nvm();
}

void calibration_service(uint16 combined)
{
    if (state != CAL_CAPTURING) { return; }

    cap_sum += (uint32_t)combined;
    cap_count++;

    if (cap_count >= CAL_CAPTURE_SAMPLES)
    {
        uint16_t avg = (uint16_t)(cap_sum / (uint32_t)cap_count);

        if (num_pts < CAL_MAX_POINTS)
        {
            pts[num_pts].pressure_bar = cap_pressure;
            pts[num_pts].counts       = avg;
            num_pts++;
        }

        state = CAL_ARMED;
    }
}

void calibration_arm(void)
{
    if (state == CAL_IDLE)
    {
        num_pts = 0u;
    }
    state = CAL_ARMED;
}

void calibration_capture(float pressure_bar)
{
    if (state != CAL_ARMED)          { return; }
    if (num_pts >= CAL_MAX_POINTS)   { return; }

    cap_pressure = pressure_bar;
    cap_sum      = 0u;
    cap_count    = 0u;
    state        = CAL_CAPTURING;
}

cal_store_result_t calibration_store(void)
{
    float prev_slope, prev_offset;

    if (num_pts < 2u)       { return CAL_STORE_TOO_FEW; }

    prev_slope  = cal_slope;
    prev_offset = cal_offset;
    if (!compute_fit())     { return CAL_STORE_BAD_FIT; }
    if (!save_to_nvm())
    {
        /* The new fit is NOT in NVM — keep the previously stored one live
         * so the output doesn't run on coefficients that vanish at reset. */
        cal_slope  = prev_slope;
        cal_offset = prev_offset;
        return CAL_STORE_NVM_FAIL;
    }

    valid = true;
    state = CAL_IDLE;
    status_led_set_state(LED_STATE_CAL_STORED);
    return CAL_STORE_OK;
}

bool calibration_clear(void)
{
    bool ok = false;

    valid      = false;
    cal_slope  = 0.0f;
    cal_offset = 0.0f;
    num_pts    = 0u;
    state      = CAL_IDLE;

    if (nvm_flash_is_healthy())
    {
        int32_t rc;

        uart_send_str("nvm: cal erase... ");   /* TEMP diag */
        uart_tx_flush_bounded();               /* TEMP diag */
        WDT1_SOW_Service(1u);
        __disable_irq();             /* atomic NVM op -- see save_to_nvm()      */
        rc = user_nvm_page_erase(CAL_NVM_ADDR);
        __enable_irq();
        (void)WDT1_Service();
        uart_send_str("rc=");                  /* TEMP diag */
        uart_send_i32(rc);
        uart_send_str("\r\n");

        ok = (rc == (int32_t)ERR_LOG_SUCCESS);
    }
    return ok;
}

void calibration_abort(void)
{
    num_pts = 0u;
    state   = CAL_IDLE;
}

bool calibration_is_valid(void)         { return valid; }
cal_state_t calibration_get_state(void) { return state; }
uint8 calibration_get_num_points(void)  { return num_pts; }
float calibration_get_slope(void)       { return cal_slope; }
float calibration_get_offset(void)      { return cal_offset; }

float calibration_apply(uint16 counts)
{
    return cal_slope * (float)counts + cal_offset;
}

/* ========================================================================= */
/*  NVM persistence                                                          */
/* ========================================================================= */
static bool load_from_nvm(void)
{
    const cal_nvm_t *nvm = (const cal_nvm_t *)(uintptr_t)CAL_NVM_ADDR;
    uint8_t i;

    if (nvm->magic != CAL_MAGIC)                               { return false; }
    if (nvm->num_points < 2u || nvm->num_points > CAL_MAX_POINTS) { return false; }

    cal_slope  = nvm->slope;
    cal_offset = nvm->offset;

    /* Restore the captured points too, so CAL STATUS reports the real count
     * after a power cycle (not "VALID pts=0") and a re-STORE stays possible. */
    num_pts = nvm->num_points;
    for (i = 0u; i < num_pts; i++)
    {
        pts[i].pressure_bar = nvm->points[i].pressure_bar;
        pts[i].counts       = nvm->points[i].counts;
    }
    return true;
}

static bool save_to_nvm(void)
{
    uint8_t page[FlashPageSize];
    cal_nvm_t *d = (cal_nvm_t *)page;
    uint8_t i;
    int32_t rc;

    if (!nvm_flash_is_healthy()) { return false; }   /* mapping inconsistent */

    /* Fill with 0xFF (erased state) */
    for (i = 0u; i < (uint8_t)FlashPageSize; i++) { page[i] = 0xFFu; }

    d->magic      = CAL_MAGIC;
    d->slope      = cal_slope;
    d->offset     = cal_offset;
    d->num_points = num_pts;
    d->reserved[0] = 0u;
    d->reserved[1] = 0u;
    d->reserved[2] = 0u;

    for (i = 0u; i < num_pts; i++)
    {
        d->points[i].pressure_bar = pts[i].pressure_bar;
        d->points[i].counts       = pts[i].counts;
        d->points[i].pad          = 0u;
    }

    /* SINGLE mapped-region write, no pre-erase. On the data-mapped sector
     * the BootROM writes a spare physical page, updates the MapRAM, and only
     * then erases the old page (UM "NVM Data Mapped Region") -- so the
     * previous cal survives a power loss mid-save, and the flash-busy window
     * is half of the old erase-then-write (which unmapped the page FIRST,
     * destroying the stored cal before the new one existed). IRQs masked:
     * the SysTick ISR lives in this same flash macro and an exception-entry
     * fetch mid-op locks up the core.                                       */
    uart_send_str("nvm: cal write... ");       /* TEMP diag */
    uart_tx_flush_bounded();                   /* TEMP diag */
    WDT1_SOW_Service(1u);
    __disable_irq();
    rc = user_nvm_write(CAL_NVM_ADDR, page, (uint32_t)FlashPageSize, 0u);
    __enable_irq();
    (void)WDT1_Service();
    uart_send_str("rc=");                      /* TEMP diag */
    uart_send_i32(rc);
    uart_send_str("\r\n");

    return (rc == (int32_t)ERR_LOG_SUCCESS);
}

/* ========================================================================= */
/*  Least-squares linear fit                                                 */
/* ========================================================================= */
static bool compute_fit(void)
{
    /*  pressure = slope * counts + offset
     *  slope  = (N*Σcp − Σc*Σp) / (N*Σc² − (Σc)²)
     *  offset = (Σp − slope*Σc) / N                                        */
    float sum_c  = 0.0f;
    float sum_p  = 0.0f;
    float sum_cc = 0.0f;
    float sum_cp = 0.0f;
    float n      = (float)num_pts;
    float denom;
    uint8_t i;

    for (i = 0u; i < num_pts; i++)
    {
        float c = (float)pts[i].counts;
        float p = pts[i].pressure_bar;
        sum_c  += c;
        sum_p  += p;
        sum_cc += c * c;
        sum_cp += c * p;
    }

    denom = n * sum_cc - sum_c * sum_c;
    if (denom == 0.0f) { return false; }

    cal_slope  = (n * sum_cp - sum_c * sum_p) / denom;
    cal_offset = (sum_p - cal_slope * sum_c) / n;
    return true;
}
