#include "nvm_config.h"
#include "app_config.h"
#include "uart_cmd.h"      /* TEMP: nvm diagnostics (rc prints) — remove with them */
#include "bootrom.h"
#include "wdt1.h"
#include "tle985x.h"
#include "sfr_access.h"

#define SETTINGS_MAGIC  0x53455402u   /* "SET\x02" (layout v2: + pressure window) */

typedef struct {
    uint32_t magic;
    uint32_t rate_ms;
    uint16_t disagree_thresh;
    uint16_t probe_mode;        /* was 'reserved' (0 = DUAL, so back-compat)  */
    float    range_lo_bar;
    float    range_hi_bar;
} settings_nvm_t;

/* ---- RAM copy ----------------------------------------------------------- */
static uint32 rate_ms;
static uint16 disagree_thresh;
static uint16 probe_mode;
static float  range_lo_bar;
static float  range_hi_bar;
static bool   flash_healthy;

/* ========================================================================= */
void nvm_config_init(void)
{
    const settings_nvm_t *nvm = (const settings_nvm_t *)(uintptr_t)SETTINGS_NVM_ADDR;

    /* BootROM UM §5.4: read the startup memory status on user-code entry.
     * MRAMINITSTS set = the data-flash MapRAM mapping is inconsistent and
     * write/erase to the data flash is NOT safe -- refuse all saves until
     * the sector is recovered (full data-sector erase + reflash).           */
    flash_healthy =
        (u1_Field_Rd32(&SCU->SYS_STRTUP_STS.reg,
                       (uint8)SCU_SYS_STRTUP_STS_MRAMINITSTS_Pos,
                       SCU_SYS_STRTUP_STS_MRAMINITSTS_Msk) == 0u);

    if (nvm->magic == SETTINGS_MAGIC)
    {
        rate_ms         = nvm->rate_ms;
        disagree_thresh = nvm->disagree_thresh;
        probe_mode      = nvm->probe_mode;
        range_lo_bar    = nvm->range_lo_bar;
        range_hi_bar    = nvm->range_hi_bar;

        /* Clamp to valid range */
        if (rate_ms < REFRESH_RATE_MIN_MS || rate_ms > REFRESH_RATE_MAX_MS)
        {
            rate_ms = REFRESH_RATE_DEFAULT_MS;
        }
        if (disagree_thresh == 0u || disagree_thresh > 1023u)
        {
            disagree_thresh = PROBE_DISAGREE_DEFAULT;
        }
        if (probe_mode > PROBE_MODE_B)
        {
            probe_mode = PROBE_MODE_DEFAULT;
        }
        /* NaN/garbage from an unwritten field fails these compares -> default */
        if (!(range_lo_bar >= RANGE_BAR_FLOOR &&
              range_hi_bar <= RANGE_BAR_CEIL &&
              (range_hi_bar - range_lo_bar) >= RANGE_MIN_SPAN_BAR))
        {
            range_lo_bar = RANGE_LO_BAR_DEFAULT;
            range_hi_bar = RANGE_HI_BAR_DEFAULT;
        }
    }
    else
    {
        rate_ms         = REFRESH_RATE_DEFAULT_MS;
        disagree_thresh = PROBE_DISAGREE_DEFAULT;
        probe_mode      = PROBE_MODE_DEFAULT;
        range_lo_bar    = RANGE_LO_BAR_DEFAULT;
        range_hi_bar    = RANGE_HI_BAR_DEFAULT;
    }
}

bool nvm_flash_is_healthy(void)
{
    return flash_healthy;
}

bool nvm_config_save(void)
{
    uint8_t page[FlashPageSize];
    settings_nvm_t *d = (settings_nvm_t *)page;
    uint8_t i;
    int32_t rc;

    if (!flash_healthy) { return false; }   /* mapping inconsistent: no writes */

    for (i = 0u; i < (uint8_t)FlashPageSize; i++) { page[i] = 0xFFu; }

    d->magic           = SETTINGS_MAGIC;
    d->rate_ms         = rate_ms;
    d->disagree_thresh = disagree_thresh;
    d->probe_mode      = probe_mode;
    d->range_lo_bar    = range_lo_bar;
    d->range_hi_bar    = range_hi_bar;

    /* SINGLE mapped-region write, no pre-erase (see calibration.c
     * save_to_nvm for the full rationale): the BootROM's used-page write
     * keeps the old page mapped until the new one is in place, so settings
     * survive a power loss mid-save, and the flash-busy window is halved.
     * IRQs masked: the SysTick ISR lives in this same flash macro and an
     * exception-entry fetch mid-op locks up the core. WDT stays safe via
     * the SOW window (no SysTick needed to service it).                     */
    uart_send_str("nvm: set write... ");       /* TEMP diag */
    uart_tx_flush_bounded();                   /* TEMP diag */
    WDT1_SOW_Service(1u);
    __disable_irq();
    rc = user_nvm_write(SETTINGS_NVM_ADDR, page, (uint32_t)FlashPageSize, 0u);
    __enable_irq();
    (void)WDT1_Service();
    uart_send_str("rc=");                      /* TEMP diag */
    uart_send_i32(rc);
    uart_send_str("\r\n");

    return (rc == (int32_t)ERR_LOG_SUCCESS);
}

uint32 nvm_config_get_rate_ms(void)        { return rate_ms; }
void   nvm_config_set_rate_ms(uint32 ms)   { rate_ms = ms; }

uint16 nvm_config_get_disagree_thresh(void)     { return disagree_thresh; }
void   nvm_config_set_disagree_thresh(uint16 t) { disagree_thresh = t; }

float  nvm_config_get_range_lo_bar(void)            { return range_lo_bar; }
float  nvm_config_get_range_hi_bar(void)            { return range_hi_bar; }
void   nvm_config_set_range_bar(float lo, float hi) { range_lo_bar = lo; range_hi_bar = hi; }

uint16 nvm_config_get_probe_mode(void)              { return probe_mode; }
void   nvm_config_set_probe_mode(uint16 m)          { probe_mode = m; }
