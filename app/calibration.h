#ifndef CALIBRATION_H
#define CALIBRATION_H

#include "types.h"
#include "app_config.h"

/*******************************************************************************
 * Multi-point linear calibration with NVM persistence.
 *
 * Flow: CAL ARM → CAL <bar> (repeat) → CAL STORE → live
 * Math: pressure_bar = slope * counts + offset  (least-squares fit)
 ******************************************************************************/

typedef enum {
    CAL_IDLE,
    CAL_ARMED,
    CAL_CAPTURING
} cal_state_t;

/* Why CAL STORE failed — so the operator isn't told "need >=2 pts" when the
 * real problem is a degenerate fit or a flash write error. */
typedef enum {
    CAL_STORE_OK,
    CAL_STORE_TOO_FEW,       /* fewer than 2 captured points                  */
    CAL_STORE_BAD_FIT,       /* degenerate fit (all points at same counts)    */
    CAL_STORE_NVM_FAIL       /* BootROM erase/write failed or flash unhealthy */
} cal_store_result_t;

void        calibration_init(void);          /* load from NVM if valid        */
void        calibration_service(uint16 combined); /* call each refresh tick   */

/* State-machine commands (driven by UART) */
void        calibration_arm(void);
void        calibration_capture(float pressure_bar);
cal_store_result_t calibration_store(void);  /* compute fit + write NVM       */
bool        calibration_clear(void);         /* erase NVM cal data; false if
                                              * the erase failed/was skipped
                                              * (cal returns after reset)     */
void        calibration_abort(void);

/* Runtime query */
bool        calibration_is_valid(void);
float       calibration_apply(uint16 counts); /* returns pressure (bar)       */
cal_state_t calibration_get_state(void);
uint8       calibration_get_num_points(void);
float       calibration_get_slope(void);
float       calibration_get_offset(void);

#endif /* CALIBRATION_H */
