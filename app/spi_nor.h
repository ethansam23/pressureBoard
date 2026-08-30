#ifndef SPI_NOR_H
#define SPI_NOR_H

#include "types.h"

/*******************************************************************************
 * Onboard NOR flash — ISSI IS25LP128F (128 Mbit / 16 MB) on SSC1.
 *
 * Signal names follow the IS25LP128F datasheet, not the generic SPI ones:
 *   CE#  chip enable   (what other vendors call CS#)
 *   SI   serial in     (MOSI — the MCU drives it, SSC1_M_MTSR on P0.4)
 *   SO   serial out    (MISO — the MCU reads it, SSC1_M_MRST on P0.5)
 *   SCK  shift clock   (SSC1_M_SCK on P0.3)
 * CE# is a plain GPIO on P1.2. Pins live in app_config.h.
 *
 * WHY SSC1 AND NOT SSC2: SSC2's master pins are SSC2_M_SCK on P1.0 ALT1 and
 * SSC2_M_MTSR on P1.1 ALT1 — exactly the downhole link's UART pins. SSC2 is
 * unusable here, permanently.
 *
 * WHY A BARE-METAL DRIVER: the Infineon RTE ships ssc_defines.h but no
 * ssc.c/ssc.h — SSC1 was never enabled in the Config Wizard. Rather than
 * regenerate the RTE (vendor code, treated read-only), this drives the
 * SSC1_Type register block at 0x48024000 directly. Zero RTE edits, same as
 * the link rearchitecture.
 *
 * THE LINK FENCE IS NOT NEEDED HERE. Flash traffic touches SSC1 and P0.3-0.5
 * only — never UART2, never P1.0 — and masks no interrupts. That is what
 * makes it fundamentally different from the BootROM user_nvm_* calls, which
 * stall IRQ-masked for 5-10 ms and therefore MUST fence (nvm_config.c).
 * The obligation instead is that bulk operations never block: see the byte
 * pump note below.
 *
 * BLOCKING POLICY (this file, today): only probe-sized transactions exist, so
 * transfers spin with a bounded guard. A 4-byte JEDEC read at 1 MHz is ~32 us
 * — negligible against the 9.2 ms packet and the ~300 ms WDT1 budget. Page
 * programs (260 bytes) and erases (tens to hundreds of ms) do NOT get this
 * treatment; they become a non-blocking state machine that returns to the
 * super-loop between chunks, with CE# held asserted across passes.
 ******************************************************************************/

/* ---- IS25LP128F command set (only what is used) -------------------------- */
#define NOR_CMD_RDID            0x9Fu   /* read JEDEC ID: mfr, type, capacity */
#define NOR_CMD_RDSR            0x05u   /* read status register (WIP is bit0) */
#define NOR_CMD_WREN            0x06u   /* write enable                       */
#define NOR_CMD_READ            0x03u   /* read data (low speed, no dummy)    */
#define NOR_CMD_PP              0x02u   /* page program, 256-byte page        */
#define NOR_CMD_SER             0x20u   /* sector erase, 4 KB                 */

/* ---- Expected JEDEC ID --------------------------------------------------- */
#define NOR_ID_MANUFACTURER     0x9Du   /* ISSI                               */
#define NOR_ID_TYPE             0x60u   /* IS25LP series                      */
#define NOR_ID_CAPACITY         0x18u   /* 128 Mbit                           */

/* ---- Geometry ------------------------------------------------------------ */
#define NOR_PAGE_SIZE           256u    /* page program granularity           */
#define NOR_SECTOR_SIZE         4096u   /* smallest erasable unit             */
#define NOR_SECTOR_COUNT        4096u   /* 4096 * 4 KB = 16 MB                */
#define NOR_TOTAL_BYTES         0x1000000u

/* ---- Status register bits ------------------------------------------------ */
#define NOR_SR_WIP              0x01u   /* write in progress                  */
#define NOR_SR_WEL              0x02u   /* write enable latch                 */

/* Configure SSC1 + pins and drive CE# inactive. Does NOT talk to the device;
 * call spi_nor_probe() for that. Safe to call before the flash has powered
 * up — no bus traffic is generated.                                         */
void spi_nor_init(void);

/* Read the JEDEC ID and latch presence. Returns true iff the ID matches
 * IS25LP128F exactly (9D 60 18).
 *
 * Tries NOR_SSC1_PH first; on mismatch retries with the opposite clock phase
 * and, if THAT answers, keeps it and records the fact (see
 * spi_nor_phase_used). The PH bit's meaning is not documented in the local
 * datasheet, so this turns an unanswerable doc question into a one-line
 * bench result. Remove the fallback once hardware settles it.               */
bool spi_nor_probe(void);

/* Last ID read by spi_nor_probe(), even when it did not match — an all-00 or
 * all-FF result is the signature of a wiring or level-shift problem rather
 * than a firmware one, so the raw bytes are worth reporting.                */
void spi_nor_get_id(uint8 out[3]);

/* True once probe() has matched. Every write path must gate on this.        */
bool spi_nor_present(void);

/* Which SSC1 clock phase actually answered (0 or 1), valid after probe().   */
uint8 spi_nor_phase_used(void);

/* True if probe() only succeeded after flipping the phase — i.e. the
 * NOR_SSC1_PH hypothesis in app_config.h is wrong and should be corrected. */
bool spi_nor_phase_was_flipped(void);

/* True if any byte transfer since the last probe timed out waiting for
 * receive-complete. This separates the two ways a probe can fail:
 *   timed out  -> the shifter never ran (clock gate, EN bit, or pin mux) —
 *                 a firmware/config fault, nothing to do with the flash
 *   completed  -> the bus ran fine and the line was simply quiet — look at
 *                 the wiring, the 3.3 V rail, CE#, and WP#/HOLD#
 * Without this, both look identical from the console.                      */
bool spi_nor_bus_timed_out(void);

/* Read the status register. Returns 0xFF if the device is not present.      */
uint8 spi_nor_read_status(void);

/* ---- Bench-only blocking operations -------------------------------------- *
 * BENCH USE ONLY — call these from console commands, never from the refresh
 * path. They block for as long as the flash takes: a page program is a few
 * ms, a sector erase is tens to hundreds. That is tolerable from a console
 * command for the same reason RAW's ~34 ms burst is (commands only run in
 * console mode, where the packet stream is suspended — nothing to tear), and
 * every wait loop feeds WDT1 and is hard-timeboxed.
 *
 * The production datalog path does NOT use these. It gets a non-blocking
 * state machine that returns to the super-loop between chunks; that is the
 * whole reason logging can run without the link fence.
 *
 * All return false on timeout or when the device is absent.                 */

/* Poll the status register until WIP clears. Feeds WDT1 while waiting.      */
bool spi_nor_wait_ready(uint32 timeout_ms);

/* Erase one 4 KB sector. `addr` may be anywhere inside it.                  */
bool spi_nor_erase_sector(uint32 addr);

/* Program up to NOR_PAGE_SIZE bytes. Must not cross a page boundary — the
 * flash wraps within the page rather than continuing, silently corrupting
 * the start of the same page.                                              */
bool spi_nor_write_page(uint32 addr, const uint8 *data, uint16 len);

/* Read any length; no erase/program timing involved.                        */
bool spi_nor_read(uint32 addr, uint8 *out, uint16 len);

/* Internal loopback self-test (CON.LB): writes four patterns and checks they
 * come back, with no external hardware in the path. Fills got[4] with what
 * was actually received. Returns true iff all four matched.
 *
 * This is the definitive split between a driver fault and a board fault:
 *   passes -> clock gate, enable, baud, data width and the RIR1/RB handling
 *             are all correct; a failing real transfer is then provably
 *             external (pin mux, translator, wiring, or the flash)
 *   fails  -> the SSC1 setup itself is wrong; stop looking at the board
 * It does NOT test bit order (the shifter unwinds its own winding) or the
 * ALT pin mux (loopback is internal).                                      */
bool spi_nor_loopback(uint8 got[4]);

/* ---- Pin-level bring-up diagnostics (LOG PINS) --------------------------- *
 * A 4-byte SPI transaction at 1 MHz is a 32 us burst — hard to catch on a
 * scope without a trigger, and invisible to a multimeter. These park the bus
 * as plain GPIO so each signal can be held at a DC level long enough to
 * measure AT THE FLASH PIN, which is what actually proves the routing.
 *
 * Sequence: diag_begin() -> diag_set()/diag_so() as needed -> diag_end().
 * diag_end() restores the full SSC1 configuration, so normal operation
 * resumes without a reset.                                                  */
#define NOR_SIG_CE              0u
#define NOR_SIG_SCK             1u
#define NOR_SIG_SI              2u

/* Bit-banged JEDEC ID read on plain GPIO at ~10 kHz, SPI mode 0. Bypasses
 * the SSC peripheral, the ALT pin mux, the baud generator, PISEL and the PH
 * hypothesis — only the pads, translators, traces and the flash are in the
 * path. Pairs with spi_nor_loopback() to bracket any fault:
 *   loopback PASS + bitbang PASS -> the ALT pin mux
 *   loopback PASS + bitbang FAIL -> wiring, translator, or flash
 *   bitbang PASS + SSC1 FAIL     -> SSC1 config
 * Fills out[3] with whatever came back; returns true iff it matched.       */
bool spi_nor_bitbang_id(uint8 out[3]);

void  spi_nor_diag_begin(void);                  /* SCK/SI -> GPIO outputs   */
void  spi_nor_diag_set(uint8 sig, bool high);    /* drive one signal         */
void  spi_nor_diag_end(void);                    /* restore SSC1             */

/* Read the SO pin under a chosen internal pull. This is the decisive test
 * for whether the flash is alive at all:
 *   pull-down reads 1 -> something EXTERNAL is driving SO high, i.e. the
 *                        flash is powered and idling — a wiring/protocol
 *                        problem, not a dead part
 *   pull-down reads 0 -> nothing is driving SO; the flash is unpowered,
 *                        absent, or its SO pin is not connected
 * (With the normal pull-up both cases read 1, which is why probing alone
 * cannot tell them apart.)                                                 */
uint8 spi_nor_diag_so(bool pull_down);

#endif /* SPI_NOR_H */
