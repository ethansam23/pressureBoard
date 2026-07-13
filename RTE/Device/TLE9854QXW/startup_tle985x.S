;/**************************************************************************//**
;
; @file     startup_tle985x.s
; @brief    CMSIS Cortex-M0 Core Device Startup File for
;           the Infineon TLE985x Device Series
; @version  V1.1.0
; @date     19. Jan 2022
; 
; Copyright (c) Infineon Technologies AG
; All rights reserved.
;
; Redistribution and use in source and binary forms, with or without modification,are permitted provided that the
; following conditions are met:
;
;   Redistributions of source code must retain the above copyright notice, this list of conditions and the  following
;   disclaimer.
;
;   Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
;   following disclaimer in the documentation and/or other materials provided with the distribution.
;
;   Neither the name of the copyright holders nor the names of its contributors may be used to endorse or promote
;   products derived from this software without specific prior written permission.
;
; THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
; INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
                                                                                
; DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE  FOR ANY DIRECT, INDIRECT, INCIDENTAL,
; SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
; SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
; WHETHER IN CONTRACT, STRICT LIABILITY,OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT  OF THE
; USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;
;*********************************************************************************************************************/

;*******************************************************************************
;                              Author(s) Identity                             **
;*******************************************************************************
;  Initials     Name                                                          **
;  ---------------------------------------------------------------------------**
;  BG           Blandine Guillot                                              **
;******************************************************************************/

;*******************************************************************************
;                           Revision Control History                          **
;*******************************************************************************
;  V1.0.0: 2017-07-07, xx:   Initial version                                  **
;  V1.1.0: 2022-01-19, BG:   Removed unused node16_IRQHandler                 **
;******************************************************************************/


; <h> Stack Configuration
;   <o> Stack Size (in Bytes) <0x0-0xFFFFFFFF:8>
; </h>

Stack_Size      EQU     0x200

                AREA    STACK, NOINIT, READWRITE, ALIGN=3
Stack_Mem       SPACE   Stack_Size
__initial_sp


; <h> Heap Configuration
;   <o>  Heap Size (in Bytes) <0x0-0xFFFFFFFF:8>
; </h>

Heap_Size       EQU     0x00000000

                AREA    HEAP, NOINIT, READWRITE, ALIGN=3
__heap_base
Heap_Mem        SPACE   Heap_Size
__heap_limit

                 THUMB
                 PRESERVE8

                
; Vector Table Mapped to Address 0 at Reset

                AREA    RESET, DATA, READONLY
                EXPORT  __Vectors
                EXPORT  __Vectors_End
                EXPORT  __Vectors_Size
                      
__Vectors       DCD     __initial_sp               ; Top of Stack
                DCD     Reset_Handler              ; Reset Handler
                DCD     NMI_Handler                ; NMI Handler
                DCD     HardFault_Handler          ; Hard Fault Handler
                DCD     MemManage_Handler          ; MPU Fault Handler
                DCD     BusFault_Handler           ; Bus Fault Handler
                DCD     UsageFault_Handler         ; Usage Fault Handler
                DCD     0                          ; Reserved
                DCD     0                          ; Reserved
                DCD     0                          ; Reserved
                DCD     0                          ; Reserved
                DCD     SVC_Handler                ; SVCall Handler
                DCD     DebugMon_Handler           ; Debug Monitor Handler
                DCD     0                          ; Reserved
                DCD     PendSV_Handler             ; PendSV Handler
                DCD     SysTick_Handler            ; SysTick Handler

                     ; External Interrupts

                DCD     GPT1_IRQHandler           ; GPTimer Interrupt 1
                DCD     GPT2_IRQHandler           ; GPTimer Interrupt 2
                DCD     ADC2_IRQHandler           ; MU interrupt / ADC2, VBG interrupt
                DCD     ADC1_IRQHandler           ; ADC 10 Bit interrupt
                DCD     CCU6SR0_IRQHandler        ; CCU6 node 0 interrupt
                DCD     CCU6SR1_IRQHandler        ; CCU6 node 1 interrupt
                DCD     CCU6SR2_IRQHandler        ; CCU6 node 2 interrupt
                DCD     CCU6SR3_IRQHandler        ; CCU6 node 3 interrupt
                DCD     SSC1_IRQHandler           ; SSC1 interrupt (receive, transmit, error)
                DCD     SSC2_IRQHandler           ; SSC2 interrupt (receive, transmit, error)
                DCD     UART1_IRQHandler          ; UART1 (ASC-LIN) interrupt (receive, transmit), T2, linsync1, LIN
                DCD     UART2_IRQHandler          ; UART2 interrupt (receive, transmit), T21, External interrupt (EINT2)
                DCD     EXINT0_IRQHandler         ; External interrupt (EINT0), wakeup
                DCD     EXINT1_IRQHandler         ; External interrupt (EINT1)
                DCD     WAKEUP_IRQHandler         ; Wakeup interrupt (generated by a wakeup event)
                DCD     DIV_IRQHandler            ; Hardware divider interrupt
                DCD     0                         ; reserved for future use
                DCD     CP_IRQHandler             ; Charge Pump interrupt
                DCD     BDRV_IRQHandler           ; Bridge Driver interrupt
                DCD     HS_IRQHandler             ; High Side Interrupt
                DCD     CSA_IRQHandler            ; OpAmp Interrupt
                DCD     DU1_IRQHandler            ; Differential Unit - DPP1
                DCD     MONx_IRQHandler           ; MONx Interrupt, wakeup
                DCD     PORT2_IRQHandler          ; Port 2.x - DPP1
__Vectors_End

__Vectors_Size EQU __Vectors_End - __Vectors

                AREA    |.text|, CODE, READONLY

;-------------------------------------------------------------------------------
; Reset Handler

Reset_Handler   PROC
                EXPORT  Reset_Handler             [WEAK]
                IMPORT  SystemInit
                IMPORT  __main
                LDR     R0, =SystemInit
                BLX     R0
                LDR     R0, =__main
                BX      R0
                ENDP

;-------------------------------------------------------------------------------
; Dummy Exception Handlers (infinite loops which can be modified)                

NMI_Handler     PROC
                EXPORT  NMI_Handler                [WEAK]
                B       .
                ENDP
HardFault_Handler\
                PROC
                EXPORT  HardFault_Handler          [WEAK]
                B       .
                ENDP
MemManage_Handler\
                PROC
                EXPORT  MemManage_Handler          [WEAK]
                B       .
                ENDP
BusFault_Handler\
                PROC
                EXPORT  BusFault_Handler           [WEAK]
                B       .
                ENDP
UsageFault_Handler\
                PROC
                EXPORT  UsageFault_Handler         [WEAK]
                B       .
                ENDP
SVC_Handler     PROC
                EXPORT  SVC_Handler                [WEAK]
                B       .
                ENDP
DebugMon_Handler\
                PROC
                EXPORT  DebugMon_Handler           [WEAK]
                B       .
                ENDP
PendSV_Handler\
                PROC
                EXPORT  PendSV_Handler             [WEAK]
                B       .
                ENDP
SysTick_Handler\
                PROC
                EXPORT  SysTick_Handler            [WEAK]
                B       .
                ENDP

Default_Handler PROC
                EXPORT  GPT1_IRQHandler           [WEAK]
                EXPORT  GPT2_IRQHandler           [WEAK]
                EXPORT  ADC2_IRQHandler           [WEAK]
                EXPORT  ADC1_IRQHandler           [WEAK]
                EXPORT  CCU6SR0_IRQHandler        [WEAK]
                EXPORT  CCU6SR1_IRQHandler        [WEAK]
                EXPORT  CCU6SR2_IRQHandler        [WEAK]
                EXPORT  CCU6SR3_IRQHandler        [WEAK]
                EXPORT  SSC1_IRQHandler           [WEAK]
                EXPORT  SSC2_IRQHandler           [WEAK]
                EXPORT  UART1_IRQHandler          [WEAK]
                EXPORT  UART2_IRQHandler          [WEAK]
                EXPORT  EXINT0_IRQHandler         [WEAK]
                EXPORT  EXINT1_IRQHandler         [WEAK]
                EXPORT  WAKEUP_IRQHandler         [WEAK]
                EXPORT  DIV_IRQHandler            [WEAK]
                EXPORT  CP_IRQHandler             [WEAK]
                EXPORT  BDRV_IRQHandler           [WEAK]
                EXPORT  HS_IRQHandler             [WEAK]
                EXPORT  CSA_IRQHandler            [WEAK]
                EXPORT  DU1_IRQHandler            [WEAK]
                EXPORT  MONx_IRQHandler           [WEAK]
                EXPORT  PORT2_IRQHandler          [WEAK]

GPT1_IRQHandler 
GPT2_IRQHandler 
ADC2_IRQHandler 
ADC1_IRQHandler 
CCU6SR0_IRQHandler 
CCU6SR1_IRQHandler 
CCU6SR2_IRQHandler 
CCU6SR3_IRQHandler 
SSC1_IRQHandler 
SSC2_IRQHandler 
UART1_IRQHandler
UART2_IRQHandler
EXINT0_IRQHandler
EXINT1_IRQHandler
WAKEUP_IRQHandler
DIV_IRQHandler
CP_IRQHandler
BDRV_IRQHandler
HS_IRQHandler
CSA_IRQHandler
DU1_IRQHandler
MONx_IRQHandler
PORT2_IRQHandler

                B       .
                ENDP


                ALIGN

;-------------------------------------------------------------------------------
; User Initial Stack & Heap

                IF      :DEF:__MICROLIB
                
                EXPORT  __initial_sp
                EXPORT  __heap_base
                EXPORT  __heap_limit
                
                ELSE
                
                IMPORT  __use_two_region_memory
                EXPORT  __user_initial_stackheap
                
__user_initial_stackheap
                LDR     R0, =  Heap_Mem
                LDR     R1, =(Stack_Mem + Stack_Size)
                LDR     R2, = (Heap_Mem +  Heap_Size)
                LDR     R3, = Stack_Mem
                BX      LR

                ALIGN

                ENDIF

                END

