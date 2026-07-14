#ifndef NVM_CONFIG_H
#define NVM_CONFIG_H

#include "types.h"

/*******************************************************************************
 * Persistent settings — NVM page at SETTINGS_NVM_ADDR.
 * Stores refresh rate, probe-disagree threshold, and probe mode.
 * (The analog-output pressure window was removed with the PWM output — the
 * digital link encodes a fixed absolute 0-1000 bar scale.)
 ******************************************************************************/

void   nvm_config_init(void);    /* load from NVM (or defaults)              */
bool   nvm_config_save(void);    /* write current values to NVM              */

/* Data-flash health, sampled once at boot (BootROM UM §5.4: read MEMSTAT /
 * MRAMINITSTS on user-code entry). When false, the MapRAM mapping is
 * inconsistent and ALL data-flash writes are refused (save functions return
 * false) until the sector is recovered.                                     */
bool   nvm_flash_is_healthy(void);

uint32 nvm_config_get_rate_ms(void);
void   nvm_config_set_rate_ms(uint32 ms);

uint16 nvm_config_get_disagree_thresh(void);
void   nvm_config_set_disagree_thresh(uint16 t);

/* Probe source: PROBE_MODE_DUAL / _A / _B */
uint16 nvm_config_get_probe_mode(void);
void   nvm_config_set_probe_mode(uint16 m);

#endif /* NVM_CONFIG_H */
