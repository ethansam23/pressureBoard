#include "nvm_config.h"
#include "app_config.h"
#include "uart_cmd.h"      /* TEMP: nvm diagnostics (rc prints) — remove with them */
#include "link_tx.h"       /* NVM fence: flash ops only in wire-idle windows  */
#include "bootrom.h"
#include "wdt1.h"
#include "tle985x.h"
#include "sfr_access.h"

#define SETTINGS_MAGIC  0x53455404u   /* "SET\x04" (v4: 12-bit-scaled counts —
                                       * a stored 10-bit threshold would be 4x
                                       * too tight; pressure window removed
                                       * with the analog output)                 */

typedef struct {
    uint32_t magic;
    uint32_t rate_ms;
    uint16_t disagree_thresh;
    uint16_t probe_mode;        /* was 'reserved' (0 = DUAL, so back-compat)  */
} settings_nvm_t;

/* ---- RAM copy ----------------------------------------------------------- */
static uint32 rate_ms;
static uint16 disagree_thresh;
static uint16 probe_mode;
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

        /* Clamp to valid range */
        if (rate_ms < REFRESH_RATE_MIN_MS || rate_ms > REFRESH_RATE_MAX_MS)
        {
            rate_ms = REFRESH_RATE_DEFAULT_MS;
        }
        if (disagree_thresh == 0u || disagree_thresh > ADC_COUNTS_MAX)
        {
            disagree_thresh = PROBE_DISAGREE_DEFAULT;
        }
        if (probe_mode > PROBE_MODE_B)
        {
            probe_mode = PROBE_MODE_DEFAULT;
        }
    }
    else
    {
        rate_ms         = REFRESH_RATE_DEFAULT_MS;
        disagree_thresh = PROBE_DISAGREE_DEFAULT;
        probe_mode      = PROBE_MODE_DEFAULT;
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

    /* SINGLE mapped-region write, no pre-erase (see calibration.c
     * save_to_nvm for the full rationale): the BootROM's used-page write
     * keeps the old page mapped until the new one is in place, so settings
     * survive a power loss mid-save, and the flash-busy window is halved.
     * IRQs masked: the SysTick ISR lives in this same flash macro and an
     * exception-entry fetch mid-op locks up the core. WDT stays safe via
     * the SOW window (no SysTick needed to service it).                     */
    uart_send_str("nvm: set write... ");       /* TEMP diag */
    uart_tx_flush_bounded();                   /* TEMP diag */
    /* FENCE (fail-closed): the ~5-10 ms IRQ-masked flash stall may only run
     * in a wire-idle window — never mid-packet. On failure, SKIP the write
     * and report it; the RAM settings are preserved.                        */
    if (!link_tx_fence_bounded()) { return false; }
    WDT1_SOW_Service(1u);
    __disable_irq();
    rc = user_nvm_write(SETTINGS_NVM_ADDR, page, (uint32_t)FlashPageSize, 0u);
    __enable_irq();
    (void)WDT1_Service();
    link_tx_release();                         /* transmits nothing; next
                                                * packet per overdue policy  */
    uart_send_str("rc=");                      /* TEMP diag */
    uart_send_i32(rc);
    uart_send_str("\r\n");

    return (rc == (int32_t)ERR_LOG_SUCCESS);
}

uint32 nvm_config_get_rate_ms(void)        { return rate_ms; }
void   nvm_config_set_rate_ms(uint32 ms)   { rate_ms = ms; }

uint16 nvm_config_get_disagree_thresh(void)     { return disagree_thresh; }
void   nvm_config_set_disagree_thresh(uint16 t) { disagree_thresh = t; }

uint16 nvm_config_get_probe_mode(void)              { return probe_mode; }
void   nvm_config_set_probe_mode(uint16 m)          { probe_mode = m; }
