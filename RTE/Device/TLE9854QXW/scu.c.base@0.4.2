/*
 ***********************************************************************************************************************
 *
 * Copyright (c) 2018-2023, Infineon Technologies AG
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

/*******************************************************************************
**                                  Includes                                  **
*******************************************************************************/
#include "scu.h"
#include "wdt1.h"
#include "lin.h"
#include "scu_defines.h"
#include "pmu_defines.h"
#include "cmsis_misra.h"

/*******************************************************************************
**                         Global Function Definitions                        **
*******************************************************************************/
void SCU_Init(void)
{
  /***************************************************************************
   ** System Clock Output Control                                           **
  ***************************************************************************/
  SCU->COCON.reg = (uint32) (SCU_COCON);

  /***************************************************************************
   ** Watchdog Control (internal)                                           **
  ***************************************************************************/
  SCU->WDTREL.reg = (uint32) SCU_WDTREL;
  SCU->WDTWINB.reg = (uint32) SCU_WDTWINB;
  
  SCU_OpenPASSWD();	
  SCU->WDTCON.reg = (uint32) SCU_WDTCON;
  SCU_ClosePASSWD();
	
  /***************************************************************************
   ** Module Pin Select                                                     **
  ***************************************************************************/
#ifdef SCU_MODPISEL
  SCU->MODPISEL.reg = (uint32) SCU_MODPISEL;
#endif
#ifdef SCU_MODPISEL1
  SCU->MODPISEL1.reg = (uint32) SCU_MODPISEL1;
#endif
#ifdef SCU_MODPISEL2
  SCU->MODPISEL2.reg = (uint32) SCU_MODPISEL2;
#endif
  SCU->GPT12PISEL.reg = (uint32) SCU_GPT12PISEL;
}

void SCU_ClkInit(void)
{
  sint32 int_was_mask;
  /* disable all interrupts                */
  int_was_mask = CMSIS_Irq_Dis();
  /***************************************************************************
  ** NVM Protection Control                                                **
  ***************************************************************************/
#if (SCU_NVM_BOOT_PROT == 1u)
  user_nvm_protect_set((uint32) SCU_NVM_PROT_PW, (NVM_PASSWORD_SEGMENT_t)SCU_NVM_BOOT_PROT);
#endif
#if (SCU_NVM_CODE_PROT == 1u)
  user_nvm_protect_set((uint32) SCU_NVM_PROT_PW, (NVM_PASSWORD_SEGMENT_t)SCU_NVM_CODE_PROT);
#endif
#if (SCU_NVM_DATA_PROT == 1u)
  user_nvm_protect_set((uint32) SCU_NVM_PROT_PW, (NVM_PASSWORD_SEGMENT_t)SCU_NVM_DATA_PROT);
#endif

  /**************************************************************************
   ** SYSCLK Control                                                   **
   **************************************************************************/
  /* select internal oscillator fINTOSC */
  SCU_OpenPASSWD();
  Field_Wrt32all(&SCU->SYSCON0.reg, (uint32)SCU_SYSCON0);
  SCU_ClosePASSWD();

  /***************************************************************************
  ** Analog Clock Control                                                  **
  ***************************************************************************/
  SCU_OpenPASSWD();
  Field_Wrt32all(&SCU->APCLK.reg, (uint32)SCU_APCLK);
  SCU_ClosePASSWD();

  /* apply settings by setting APCLK_SET */
  /* APCLK_SET is cleared by HW once the clock settings are overtaken */
  SCU_OpenPASSWD();
  Field_Mod32(&SCU->APCLK_CTRL.reg, (uint8)SCU_APCLK_CTRL_APCLK_SET_Pos, SCU_APCLK_CTRL_APCLK_SET_Msk, 1u);
  SCU_ClosePASSWD();

  /* enable interrupts                     */
  if (int_was_mask == 0)
  {
    CMSIS_Irq_En();
  }
}

#if (PMU_SLEEP_MODE == 1u)
/* violation: Last value assigned to variable 'dummy' (defined at line 282) not used [MISRA 2012 Rule 2.2, required] */
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-noreturn"
#endif
void SCU_EnterSleepMode(void)
{
  /* violation: Previously assigned value to variable 'dummy' has not been used */
  /* This function assumes the desired     *
  * wake up sources are enabled already   */
  uint32 dummy;
  /* to remove compiler warning of unused  *
  * dummy variable                        */
  dummy = 0;
  dummy = dummy;
  /* set LIN Trx into Sleep Mode if LIN is enabled */
  if(LIN_Get_Mode() != LIN_GET_MODE_DISABLED)
  {
    LIN_Set_Mode(LIN_MODE_SLEEP);
    /* wait until LIN Trx switched           *
    * into Sleep Mode                       */
    while ((LIN_Get_Mode() != LIN_GET_MODE_SLEEP))
    {
    }
    /* wait approx. 100µs in order to pass   *
    * the filter time + margin inside the   *
    * LIN Trx. to avoid a false wake up     */
    Delay_us(100u);    
  }

  /* dummy read to clr wake up status      *
  * to prevent the device from wakeing up *
  * immediately because of still set      *
  * wake up flags                         */
  dummy = u32_Field_Rd32(&PMU->WAKE_STATUS.reg, (uint8)PMU_WAKE_STATUS_LIN_WAKE_Pos, MASK_WORD);

  /* stop WDT1 (SysTick) to prevent any    *
  * SysTick interrupt to disturb the      *
  * Sleep Mode Entry sequence             */
  WDT1_Stop();
  /* Trigger a ShortOpenWindow on WDT1     *
  * to prevent an unserviced WDT1 by      *
  * accident                              */
  WDT1_SOW_Service(1u);
  /* disable all interrupts                */
  (void)CMSIS_Irq_Dis();
  /* open passwd to access PMCON0 register */
  SCU_OpenPASSWD();
  /* set SleepMode flag in PMCON0        */
  Field_Wrt32all(&SCU->PMCON0.reg, (uint32)0x02);
  CMSIS_WFE();
  CMSIS_WFE();
  /* dont do anything anymore            *
  * device is in SleepMode              */
#ifndef UNIT_TESTING_LV2
  for (;;)
  {
  }
#endif
  /* device is in Sleep Mode now          *
  * wake up performs RESET               */
}
#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6000000)
#pragma clang diagnostic pop
#endif
#endif

#if (PMU_STOP_MODE == 1u)
/* violation: Last value assigned to variable 'dummy' (defined at line 346) not used [MISRA 2012 Rule 2.2, required] */
void SCU_EnterStopMode(void)
{
  sint32 int_was_mask;
  /* violation: Previously assigned value to variable 'dummy' has not been used */
  uint32 dummy;
  /* to remove compiler warning of unused  *
  * dummy variable                        */
  dummy = 0;
  dummy = dummy;
  /* dummy read to clr wake up status      *
  * to prevent the device from wakeing up *
  * immediately because of still set      *
  * wake up flags                         */
  dummy = u32_Field_Rd32(&PMU->WAKE_STATUS.reg, (uint8)PMU_WAKE_STATUS_LIN_WAKE_Pos, MASK_WORD);
  /* dummy read to clr GPIO wake up status */
  dummy = u32_Field_Rd32(&PMU->GPIO_WAKE_STATUS.reg, 0, 0xFFFFFFFF);
  /* stop WDT1 (SysTick) to prevent any    *
  * SysTick interrupt to disturb the      *
  * Sleep Mode Entry sequence             */
  WDT1_Stop();
  /* Trigger a ShortOpenWindow on WDT1     *
  * to prevent an unserviced WDT1 by      *
  * accident                              */
  WDT1_SOW_Service(1u);
  /* disable all interrupts                */
  int_was_mask = CMSIS_Irq_Dis();

  /* select LP_CLK as sys clock            */
  SCU_OpenPASSWD();
  Field_Mod32(&SCU->SYSCON0.reg, (uint8)SCU_SYSCON0_SYSCLKSEL_Pos, (uint32)SCU_SYSCON0_SYSCLKSEL_Msk, 2U);
  SCU_ClosePASSWD();

  /* open passwd to access PMCON0 register */
  SCU_OpenPASSWD();

  /* set PowerDown flag in PMCON0        */
  Field_Wrt32all(&SCU->PMCON0.reg, (uint32)0x04);
  CMSIS_WFE();
  CMSIS_WFE();
  CMSIS_NOP();
  CMSIS_NOP();
  CMSIS_NOP();
  CMSIS_NOP();

  /* dont do anything anymore            *
  * device is in PowerDown Mode         */
  /***************************************/
  /********* DEVICE IN STOP MODE *********/
  /***************************************/
  /* Device has been woken up            */	
  /* select fINTOSC as sys clock            */
  SCU_OpenPASSWD();
  Field_Mod32(&SCU->SYSCON0.reg, (uint8)SCU_SYSCON0_SYSCLKSEL_Pos, (uint32)SCU_SYSCON0_SYSCLKSEL_Msk, 3U);
  SCU_ClosePASSWD();
  /* enable interrupts                     */
  if (int_was_mask == 0)
  {
    CMSIS_Irq_En();
  }

  /* Init SysTick                          */
  SysTick_Init();

  /* Init and service WDT1                 */
  WDT1_Init();
}
#endif

int32_t SCU_ChangeNVMProtection(NVM_PASSWORD_SEGMENT_t Protection, uint8 Action)
{
  int32_t result;
  result = (int32_t)ERR_LOG_ERROR;
  if (Action == PROTECTION_CLEAR)
  {
    if (Protection == NVM_PASSWORD_SEGMENT_BOOT)
    {
      result = (int32_t)user_nvm_protect_clear((uint32) SCU_NVM_PROT_PW, Protection);
    }
    else if (Protection == NVM_PASSWORD_SEGMENT_CODE)
    {
      result = (int32_t)user_nvm_protect_clear((uint32) SCU_NVM_PROT_PW, Protection);
    }
    else if (Protection == NVM_PASSWORD_SEGMENT_DATA)
    {
      result = (int32_t)user_nvm_protect_clear((uint32) SCU_NVM_PROT_PW, Protection);
    }
    else
    {

    }
  }
  else
  {
    if (Protection == NVM_PASSWORD_SEGMENT_BOOT)
    {
      result = (int32_t)user_nvm_protect_set((uint32) SCU_NVM_PROT_PW, Protection);
    }
    else if (Protection == NVM_PASSWORD_SEGMENT_CODE)
    {
      result = (int32_t)user_nvm_protect_set((uint32) SCU_NVM_PROT_PW, Protection);
    }
    else if (Protection == NVM_PASSWORD_SEGMENT_DATA)
    {
      result = (int32_t)user_nvm_protect_set((uint32) SCU_NVM_PROT_PW, Protection);
    }
    else
    {

    }
  }
  return (result);
}

