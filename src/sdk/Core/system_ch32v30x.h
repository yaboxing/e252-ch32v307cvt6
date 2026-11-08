/********************************** (C) COPYRIGHT *******************************
* File Name          : system_ch32v30x.h
* Author             : WCH
* Version            : V1.0.0
* Date               : 2021/06/06
* Description        : CH32V30x Device Peripheral Access Layer System Header File.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#ifndef __SYSTEM_CH32V30x_H 
#define __SYSTEM_CH32V30x_H

#ifdef __cplusplus
 extern "C" {
#endif 

extern uint32_t SystemCoreClock;          /* System Clock Frequency (Core Clock) */

/* System_Exported_Functions */  
extern void SystemInit(void);
extern void SystemCoreClockUpdate(void);

/* 时钟启动诊断（在 SystemInit 内填充，供 main/应用层读取） */
extern volatile uint32_t g_ClkDiagAttempts;    /* HSE 尝试次数 */
extern volatile uint32_t g_ClkDiagOkAttempt;   /* 第几次尝试成功（0=全部失败） */
extern volatile uint32_t g_ClkDiagOkLoops;     /* 成功那次等待 HSERDY 的循环数 */
extern volatile uint32_t g_ClkDiagPllLoops;    /* 等待 PLLRDY 的循环数 */
extern volatile uint32_t g_ClkDiagFallback;    /* 1=HSE 不可用，已走 HSI 兜底 */
extern volatile uint32_t g_ClkDiagPath;        /* 0=HSE+PLL 96MHz 1=HSI+PLL 96MHz 2=HSI 直出 8MHz */
extern volatile uint32_t g_ClkDiagValid;       /* 稳定窗口后四项复检位图，0x0F=通过 */
extern volatile uint32_t g_HseLoops[5];        /* 每次尝试等待 HSERDY 的循环数（0=超时） */

#ifdef __cplusplus
}
#endif

#endif /*__CH32V30x_SYSTEM_H */



