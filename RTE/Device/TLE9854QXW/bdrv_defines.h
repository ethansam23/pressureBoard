/**
 * @cond
 ***********************************************************************************************************************
 *
 * Copyright (c) 2018, Infineon Technologies AG
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
#ifndef BDRV_DEFINES_H
#define BDRV_DEFINES_H

/* XML Version 1.3.1 */
#define BDRV_XML_VERSION (10301)

#define BDRV_ASEQC (0x0) /*decimal 0*/

#define BDRV_ASEQIOFFMAX (0x3F) /*decimal 63*/

#define BDRV_ASEQIOFFMAX_I1OFFMAX_Value (0x140) /*decimal 320*/

#define BDRV_ASEQIOFFMIN (0x0) /*decimal 0*/

#define BDRV_ASEQIOFFMIN_I1OFFMIN_Value (0x7) /*decimal 7*/

#define BDRV_ASEQIONMAX (0x3F) /*decimal 63*/

#define BDRV_ASEQIONMAX_I1ONMAX_Value (0x140) /*decimal 320*/

#define BDRV_ASEQIONMIN (0x0) /*decimal 0*/

#define BDRV_ASEQIONMIN_I1ONMIN_Value (0x7) /*decimal 7*/

#define BDRV_CPCLKDIS_En (0x0) /*decimal 0*/

#define BDRV_CP_CLK (0xFA) /*decimal 250*/

#define BDRV_CP_CLK_CTRL (0xD21F) /*decimal 53791*/

#define BDRV_CP_CLK_SRC (0x1312D00) /*decimal 20000000*/

#define BDRV_CP_CTRL (0x10020000) /*decimal 268566528*/

#define BDRV_CP_IRQEN (0x0) /*decimal 0*/

#define BDRV_CTRL2 (0x0) /*decimal 0*/

#define BDRV_CTRL3 (0x10000) /*decimal 65536*/

#define BDRV_HB1OFF_MODE (0x0) /*decimal 0*/

#define BDRV_HB1ON_MODE (0x0) /*decimal 0*/

#define BDRV_HB2OFF_MODE (0x0) /*decimal 0*/

#define BDRV_HB2ON_MODE (0x0) /*decimal 0*/

#define BDRV_IGATECLMPOFFC (0x707) /*decimal 1799*/

#define BDRV_IGATECLMPOFFC_HB1_ICLMPOFF_Value (0x17) /*decimal 23*/

#define BDRV_IGATECLMPOFFC_HB2_ICLMPOFF_Value (0x17) /*decimal 23*/

#define BDRV_IGATECLMPONC (0x707) /*decimal 1799*/

#define BDRV_IGATECLMPONC_HB1_ICLMPON_Value (0x17) /*decimal 23*/

#define BDRV_IGATECLMPONC_HB2_ICLMPON_Value (0x17) /*decimal 23*/

#define BDRV_IGATECLMPONOFFC_HBx_ICLMPOFF_Value (0x17) /*decimal 23*/

#define BDRV_IGATECLMPONOFFC_HBx_ICLMPON_Value (0x17) /*decimal 23*/

#define BDRV_IRQEN (0x0) /*decimal 0*/

#define BDRV_LO_DITH (210.53)

#define BDRV_LO_DITH_FREQ (0xD1) /*decimal 209*/

#define BDRV_MODE (0x0) /*decimal 0*/

#define BDRV_OFFASEQTMIN (0x0) /*decimal 0*/

#define BDRV_OFFASEQTMIN_HB1T1OFFADDDLY_Value (0x0) /*decimal 0*/

#define BDRV_OFFASEQTMIN_HB2T1OFFADDDLY_Value (0x0) /*decimal 0*/

#define BDRV_OFFSEQHB1IC (0x20030320) /*decimal 537068320*/

#define BDRV_OFFSEQHB1IC_HB1_I1OFF_Value (0x84) /*decimal 132*/

#define BDRV_OFFSEQHB1IC_HB1_I2OFF_Value (0xC) /*decimal 12*/

#define BDRV_OFFSEQHB1IC_HB1_I3OFF_Value (0xC) /*decimal 12*/

#define BDRV_OFFSEQHB1IC_HB1_I4OFF_Value (0x84) /*decimal 132*/

#define BDRV_OFFSEQHB1TC (0x3020206) /*decimal 50463238*/

#define BDRV_OFFSEQHB1TC_HB1_T1OFF_Value (0x15E) /*decimal 350*/

#define BDRV_OFFSEQHB1TC_HB1_T2OFF_Value (0x96) /*decimal 150*/

#define BDRV_OFFSEQHB1TC_HB1_T3OFF_Value (0x96) /*decimal 150*/

#define BDRV_OFFSEQHB1TC_HB1_T4OFF_Value (0xC8) /*decimal 200*/

#define BDRV_OFFSEQHB2IC (0x20030320) /*decimal 537068320*/

#define BDRV_OFFSEQHB2IC_HB2_I1OFF_Value (0x84) /*decimal 132*/

#define BDRV_OFFSEQHB2IC_HB2_I2OFF_Value (0xC) /*decimal 12*/

#define BDRV_OFFSEQHB2IC_HB2_I3OFF_Value (0xC) /*decimal 12*/

#define BDRV_OFFSEQHB2IC_HB2_I4OFF_Value (0x84) /*decimal 132*/

#define BDRV_OFFSEQHB2TC (0x3020206) /*decimal 50463238*/

#define BDRV_OFFSEQHB2TC_HB2_T1OFF_Value (0x15E) /*decimal 350*/

#define BDRV_OFFSEQHB2TC_HB2_T2OFF_Value (0x96) /*decimal 150*/

#define BDRV_OFFSEQHB2TC_HB2_T3OFF_Value (0x96) /*decimal 150*/

#define BDRV_OFFSEQHB2TC_HB2_T4OFF_Value (0xC8) /*decimal 200*/

#define BDRV_ONOFFSEQHBxIC_HBx_I1OFF_Value (0x84) /*decimal 132*/

#define BDRV_ONOFFSEQHBxIC_HBx_I1ON_Value (0x84) /*decimal 132*/

#define BDRV_ONOFFSEQHBxTC_HBx_T1OFF_Value (0x15E) /*decimal 350*/

#define BDRV_ONOFFSEQHBxTC_HBx_T1ON_Value (0x15E) /*decimal 350*/

#define BDRV_ONSEQHB1IC (0x20030320) /*decimal 537068320*/

#define BDRV_ONSEQHB1IC_HB1_I1ON_Value (0x84) /*decimal 132*/

#define BDRV_ONSEQHB1IC_HB1_I2ON_Value (0xC) /*decimal 12*/

#define BDRV_ONSEQHB1IC_HB1_I3ON_Value (0xC) /*decimal 12*/

#define BDRV_ONSEQHB1IC_HB1_I4ON_Value (0x84) /*decimal 132*/

#define BDRV_ONSEQHB1TC (0x3020206) /*decimal 50463238*/

#define BDRV_ONSEQHB1TC_HB1_T1ON_Value (0x15E) /*decimal 350*/

#define BDRV_ONSEQHB1TC_HB1_T2ON_Value (0x96) /*decimal 150*/

#define BDRV_ONSEQHB1TC_HB1_T3ON_Value (0x96) /*decimal 150*/

#define BDRV_ONSEQHB1TC_HB1_T4ON_Value (0xC8) /*decimal 200*/

#define BDRV_ONSEQHB2IC (0x20030320) /*decimal 537068320*/

#define BDRV_ONSEQHB2IC_HB2_I1ON_Value (0x84) /*decimal 132*/

#define BDRV_ONSEQHB2IC_HB2_I2ON_Value (0xC) /*decimal 12*/

#define BDRV_ONSEQHB2IC_HB2_I3ON_Value (0xC) /*decimal 12*/

#define BDRV_ONSEQHB2IC_HB2_I4ON_Value (0x84) /*decimal 132*/

#define BDRV_ONSEQHB2TC (0x3020206) /*decimal 50463238*/

#define BDRV_ONSEQHB2TC_HB2_T1ON_Value (0x15E) /*decimal 350*/

#define BDRV_ONSEQHB2TC_HB2_T2ON_Value (0x96) /*decimal 150*/

#define BDRV_ONSEQHB2TC_HB2_T3ON_Value (0x96) /*decimal 150*/

#define BDRV_ONSEQHB2TC_HB2_T4ON_Value (0xC8) /*decimal 200*/

#define BDRV_OUT_VOLT (0x1) /*decimal 1*/

#define BDRV_PWMSRCSEL (0x8001A) /*decimal 524314*/

#define BDRV_SEQAFHB1CD (0x4040) /*decimal 16448*/

#define BDRV_SEQAFHB1CD_HB1_AF_OFF_Value (0xCB2) /*decimal 3250*/

#define BDRV_SEQAFHB1CD_HB1_AF_ON_Value (0xCB2) /*decimal 3250*/

#define BDRV_SEQAFHB1IC (0x70007) /*decimal 458759*/

#define BDRV_SEQAFHB1IC_HB1AF_IOFF_Value (0x17) /*decimal 23*/

#define BDRV_SEQAFHB1IC_HB1AF_ION_Value (0x17) /*decimal 23*/

#define BDRV_SEQAFHB2CD (0x4040) /*decimal 16448*/

#define BDRV_SEQAFHB2CD_HB2_AF_OFF_Value (0xCB2) /*decimal 3250*/

#define BDRV_SEQAFHB2CD_HB2_AF_ON_Value (0xCB2) /*decimal 3250*/

#define BDRV_SEQAFHB2IC (0x70007) /*decimal 458759*/

#define BDRV_SEQAFHB2IC_HB2AF_IOFF_Value (0x17) /*decimal 23*/

#define BDRV_SEQAFHB2IC_HB2AF_ION_Value (0x17) /*decimal 23*/

#define BDRV_SEQMAP (0x0) /*decimal 0*/

#define BDRV_SHOW_REG_VALS (0x0) /*decimal 0*/

#define BDRV_TRIM_DRVx (0x0) /*decimal 0*/

#define BDRV_UP_DITH (243.9)

#define BDRV_UP_DITH_FREQ (0xF3) /*decimal 243*/

#define SCU_BDRV_CLK (0x14) /*decimal 20*/

#define SCU_BRDRV_CLK (0x901) /*decimal 2305*/

#define SCU_BRDRV_TFILT_CLK (0x4) /*decimal 4*/

#endif /* BDRV_DEFINES_H */
