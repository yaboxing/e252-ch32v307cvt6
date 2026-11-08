/**
\file el9800hw.h
\brief CH32V307 + E252 (LAN9252 兼容 PDI) 端口层定义

SSC 5.11 平台适配层：保留 EL9800 应用与 SSC 栈的硬件访问契约，
ESC 访问通过 E252 的 CSR/PRAM 间接 PDI 实现。
*/

#ifndef _EL9800HW_H_
#define _EL9800HW_H_

#include "esc.h"
#include "ch32v30x.h"
#include "bsp/board_config.h"

/*-----------------------------------------------------------------------------------------
------    Defines and Types
-----------------------------------------------------------------------------------------*/
#define ESC_RD                  0x02   /**< \brief Indicates a read access to ESC or EEPROM */
#define ESC_WR                  0x04   /**< \brief Indicates a write access to ESC or EEPROM */

/*---------------------------------------------
-    Hardware timer settings
-----------------------------------------------*/
/* TIM3 计数频率 2 MHz（board_config.h: ECAT_TIMER_PSC），故 1 ms = 2000 ticks */
#define ECAT_TIMER_INC_P_MS     2000UL

/*---------------------------------------------
-    Interrupt and Timer defines
-----------------------------------------------*/
#ifndef DISABLE_ESC_INT
#define DISABLE_ESC_INT()       NVIC_DisableIRQ(EXTI3_IRQn)
#endif
#ifndef ENABLE_ESC_INT
#define ENABLE_ESC_INT()        NVIC_EnableIRQ(EXTI3_IRQn)
#endif

#ifndef HW_GetTimer
#define HW_GetTimer()           TIM3->CNT
#endif
#ifndef HW_ClearTimer
#define HW_ClearTimer()         (TIM3->CNT = 0)
#endif

/*---------------------------------------------
-    ESC access macros
-----------------------------------------------*/
#define HW_EscReadByte(ByteValue, Address)      HW_EscRead(((MEM_ADDR *)&(ByteValue)), ((UINT16)(Address)), 1)
#define HW_EscReadWord(WordValue, Address)      HW_EscRead(((MEM_ADDR *)&(WordValue)), ((UINT16)(Address)), 2)
#define HW_EscReadDWord(DWordValue, Address)    HW_EscRead(((MEM_ADDR *)&(DWordValue)), ((UINT16)(Address)), 4)
#define HW_EscReadMbxMem(pData, Address, Len)   HW_EscRead(((MEM_ADDR *)(pData)), ((UINT16)(Address)), (Len))

#define HW_EscReadByteIsr(ByteValue, Address)   HW_EscReadIsr(((MEM_ADDR *)&(ByteValue)), ((UINT16)(Address)), 1)
#define HW_EscReadWordIsr(WordValue, Address)   HW_EscReadIsr(((MEM_ADDR *)&(WordValue)), ((UINT16)(Address)), 2)
#define HW_EscReadDWordIsr(DWordValue, Address) HW_EscReadIsr(((MEM_ADDR *)&(DWordValue)), ((UINT16)(Address)), 4)

#define HW_EscWriteByte(ByteValue, Address)     HW_EscWrite(((MEM_ADDR *)&(ByteValue)), ((UINT16)(Address)), 1)
#define HW_EscWriteWord(WordValue, Address)     HW_EscWrite(((MEM_ADDR *)&(WordValue)), ((UINT16)(Address)), 2)
#define HW_EscWriteDWord(DWordValue, Address)   HW_EscWrite(((MEM_ADDR *)&(DWordValue)), ((UINT16)(Address)), 4)
#define HW_EscWriteMbxMem(pData, Address, Len)  HW_EscWrite(((MEM_ADDR *)(pData)), ((UINT16)(Address)), (Len))

#define HW_EscWriteWordIsr(WordValue, Address)  HW_EscWriteIsr(((MEM_ADDR *)&(WordValue)), ((UINT16)(Address)), 2)
#define HW_EscWriteDWordIsr(DWordValue, Address) HW_EscWriteIsr(((MEM_ADDR *)&(DWordValue)), ((UINT16)(Address)), 4)

/*-----------------------------------------------------------------------------------------
------    Diagnostics（接口链路可观测性，只读计数，不改变协议行为）
-----------------------------------------------------------------------------------------*/
typedef struct
{
    UINT32 Exti3Count;       /* EXTI3_IRQHandler 进入次数 */
    UINT32 Exti3NoFlag;      /* 进入时 EXTI3 挂起位已清的次数（伪中断） */
    UINT32 Exti3PollModeCount; /* FreeRun 下仅清挂起、不消费 AL Event 的次数 */
    UINT32 PdiIsrCount;      /* PDI_Isr 调用次数 */
    UINT32 AlEventReadCount; /* 0x0220 读取次数（ISR + 主循环） */
    UINT32 OutEventCount;    /* AL Event bit10（SCU2/输出）命中次数 */
    UINT32 InEventCount;     /* AL Event bit11（SCU3/输入）命中次数 */
    UINT32 FirstEventTick;   /* 首次非零 AL Event 的 1ms tick */
    UINT32 FirstOutTick;     /* 首次输出事件 tick */
    UINT32 LastOutTick;      /* 最近一次输出事件 tick */
    UINT32 WdErrCount;       /* SSC 判定 PD 看门狗超时的次数 */
    UINT32 WdErrSinceOut;    /* 报错时刻距最近一次输出事件的毫秒数 */
    UINT16 WdErrStatus;      /* 报错时刻读到的 0x0440 原始值 */
    UINT16 AlEventLast;      /* 最近一次 0x0220 低 16 位原始值 */
    UINT8  IrqLevelIsr;      /* 中断入口时 PA3 电平（0=低=ESC 请求中） */
    UINT8  IrqLevelIdle;     /* 主循环采样 PA3 电平 */

    /* EXTI3 入口 -> 读走 AL Event 的耗时（TIM2 96 MHz，1 us = 96 ticks） */
    UINT32 IsrLatN;
    UINT32 IsrLatSumTicks;
    UINT32 IsrLatMinTicks;
    UINT32 IsrLatMaxTicks;
    UINT32 IsrLatHist[8];    /* <=1/2/4/8/16/32/64/>64 us */

    /* PDI_Isr 退出时的中断状态观测（INT_STS bit0 为 AL 事件的只读镜像，bit30 READY 为 R/WC） */
    UINT32 IntStsAtExit;     /* PDI_Isr 之后读到的 INT_STS 原值 */
    UINT32 IntStsEcatStuck;  /* PDI_Isr 之后 bit0 仍为 1 的次数（源未清，IRQ 仍有效） */
    UINT32 IrqLowAtExit;     /* 已等到上限 PA3 仍为低的次数 */
    UINT32 IrqDeassertTicks; /* PA3 由中断入口回到空闲高所需的最大 tick（96 MHz） */

    /* DC / SYNC 边沿统计：周期单位为 TIM2 96 MHz tick，已用 TIM3 2 MHz 解回绕 */
    UINT32 Sync0EdgeCount;
    UINT32 Sync1EdgeCount;
    UINT32 Sync0PeriodMinTicks;
    UINT32 Sync0PeriodMaxTicks;
    UINT32 Sync0PeriodSumTicks;
    UINT32 Sync1PeriodMinTicks;
    UINT32 Sync1PeriodMaxTicks;
    UINT32 Sync1PeriodSumTicks;
    UINT32 Sync1PhaseMinTicks;   /* SYNC1 边沿相对最近一次 SYNC0 边沿的相位 */
    UINT32 Sync1PhaseMaxTicks;
    INT32  Sync1PhaseSumTicks;
    UINT32 Sync1LastPhaseTicks;  /* 最近一次 SYNC1 相位（瞬时值，非累计平均） */
    UINT32 Sync1LastPeriodTicks;
    UINT32 Sync1InvalidCount;    /* 首次或间距 > 32.7 ms（TIM3 回绕）的边沿次数 */
    UINT32 Sync1PhaseN;          /* 相位有效样本数（avg 分母） */

    /* TIM4 1 ms 中断的实际间隔（TIM3 2 MHz tick，1 tick = 0.5 us）。
     * 用于判定“软件 1 ms tick”是否等于真实 1 ms（中断被长临界区延迟时会变慢）。 */
    UINT32 Tim4DtN;
    UINT32 Tim4DtMinTicks;
    UINT32 Tim4DtMaxTicks;
    UINT32 Tim4DtSumTicks;
} HW_DIAG;

#endif /* _EL9800HW_H_ */

/*-----------------------------------------------------------------------------------------
------    Global functions
-----------------------------------------------------------------------------------------*/
#if EL9800_HW
    #define PROTO
#else
    #define PROTO extern
#endif

PROTO UINT8 HW_Init(void);
PROTO void HW_Release(void);
PROTO UINT16 HW_GetALEventRegister(void);
PROTO UINT16 HW_GetALEventRegister_Isr(void);
PROTO void HW_SetLed(UINT8 RunLed, UINT8 ErrLed);
PROTO void HW_EscRead(MEM_ADDR *pData, UINT16 Address, UINT16 Len);
PROTO void HW_EscReadIsr(MEM_ADDR *pData, UINT16 Address, UINT16 Len);
PROTO void HW_EscWrite(MEM_ADDR *pData, UINT16 Address, UINT16 Len);
PROTO void HW_EscWriteIsr(MEM_ADDR *pData, UINT16 Address, UINT16 Len);

HW_DIAG *HW_GetDiag(void);
void HW_NoteWdError(UINT16 status);
void HW_IrqPolarityProbe(UINT8 *levelNormal, UINT8 *levelInverted, UINT32 *cfgNormal);
void HW_IrqEdgeProbe(UINT32 *edgesInv, UINT32 *edgesNormal,
                     UINT8 *levelInv, UINT8 *levelNormal);
void HW_GetSync1Hist(UINT16 hist[8]);
UINT32 HW_GetSync0AvgTicks(void);
UINT32 HW_GetSync1AvgTicks(void);
void HW_ResetSyncStats(void);

#undef PROTO
