/*
 ***********************************************************************************************************************
 *
 * Copyright (c) 2017-2022, Infineon Technologies AG
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,are permitted provided that the
 * following conditions are met:
 *
 *   Redistributions of source code must retain the above copyright notice, this list of conditions and the  following
 *   disclaimer.
 *
 *   Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *   following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 *   Neither the name of the copyright holders nor the names of its contributors may be used to endorse or promote
 *   products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE  FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY,OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT  OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **********************************************************************************************************************/
/**
 * \file     system_tle985x.h
 *
 * \brief    System file for TLE985x
 *
 * \version  V0.2.2
 * \date     26. Apr 2022
 *
 * \note This file violates [MISRA 2012 Rule 11.1, required], [MISRA 2012 Rule 11.4, advisory], [MISRA 2012 Rule 11.6, required], [MISRA 2012 Directive 2.1, required], [MISRA 2012 Rule 1.2, advisory]
 */

/*******************************************************************************
**                             Author(s) Identity                             **
********************************************************************************
** Initials     Name                                                          **
** ---------------------------------------------------------------------------**
** DM           Daniel Mysliwitz                                              **
** BG           Blandine Guillot                                              **
*******************************************************************************/

/*******************************************************************************
**                          Revision Control History                          **
********************************************************************************
** V0.1.0: 2017-07-07, DM:   Initial version                                  **
** V0.1.1: 2019-02-06, DM:   XTAL, OSC_CLK, FSYS defines removed              **
**                           _NOP_ define removed                             **
**                           SysTick_Init() added to SystemInit() function    **
** V0.1.2: 2020-03-02, BG:   Updated revision history format                  **
** V0.1.3: 2020-07-28, JO:   EP-431: Added extern declaration for p_NACNAD    **
**                           to remove ARMCC v6 warning                       **
**                           Changed type of SystemFrequency from uint32_t to **
**                           uint32                                           **
**                           Added include of system_tle985x.h to             **
**                           system_tle985x.c                                 **
** V0.2.0: 2022-01-19, BG:   EP-904: Added compatibility with IAR compiler    **
** V0.2.1: 2022-01-21, JO:   EP-934: Updated copyright and branding           **
** V0.2.2: 2022-04-26, JO:   EP-844: Fixed NACNAD placement when using ARMCC  **
**                           v6 and LTO by adding attribute used              **
*******************************************************************************/

#ifndef __SYSTEM_TLE985x_H
#define __SYSTEM_TLE985x_H

/*******************************************************************************
**                                  Includes                                  **
*******************************************************************************/
#include "types.h"

#ifdef __cplusplus
 extern "C" {
#endif 

/*******************************************************************************
**                        Global Variable Definitions                         **
*******************************************************************************/

extern uint32 SystemFrequency;    /*!< System Clock Frequency (Core Clock)  */
extern const uint32 p_NACNAD;

/*******************************************************************************
**                         Global Function Declarations                       **
*******************************************************************************/

/** \brief Initialize System, clock and watchdog1. 
 */
void SystemInit (void);

#ifdef __cplusplus
}
#endif

#endif /* __SYSTEM_TLE985x_H */
