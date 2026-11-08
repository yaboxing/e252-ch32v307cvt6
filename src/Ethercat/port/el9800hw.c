/**
\file el9800hw.c
\brief CH32V307 + E252 端口层实现（SSC 5.11）

ESC 访问走 E252 的 Direct CSR/PRAM PDI（hal/lan9252_pdi_access.c），
平台钩子 PDI_PortRead32/PDI_PortWrite32 由 hal/e252_spi.c 提供。
*/

#include "ecat_def.h"

#if EL9800_HW

#include "hal/lan9252_pdi_access.h"
#include "hal/e252_spi.h"
#include "bsp/board_config.h"
#include "Ethercat/port/el9800hw.h"
#include "Ethercat/Inc/ecatappl.h"
#include "bsp/ch32v307_bsp.h"

/* E252 寄存器偏移（主机接口） */
#define E252_REG_ID_REV          0x0050U
#define E252_REG_IRQ_CFG         0x0054U
#define E252_REG_INT_STS         0x0058U
#define E252_REG_INT_EN          0x005CU
/* INT_STS 位定义（SYS-E252 数据手册 §9.3.2）：bit0 为 EtherCAT 中断事件，
 * 只读镜像，必须先把 AL 事件源（0x0220）清掉才会归零；bit30 为 READY 锁存，
 * R/WC，上电后一直为 1，写 1 清除（INT_EN.bit30=0，不驱动 IRQ） */
#define E252_INT_STS_ECAT        0x00000001UL
#define E252_INT_STS_READY       0x40000000UL
/* IRQ 引脚去断言等待上限：10 µs（tick 频率随主频变化，见 BOARD_TICKS_PER_US） */
#define E252_IRQ_DEASSERT_TICKS  (BOARD_TICKS_PER_US * 10U)
#define E252_REG_BYTE_TEST       0x0064U
#define E252_REG_RESET_CTL       0x01F8U

/* AL Event / AL Event Mask 偏移由 esc.h 提供，编译期核对数值 */
#if (ESC_AL_EVENT_OFFSET != 0x0220)
#error "ESC_AL_EVENT_OFFSET mismatch (expected 0x0220)"
#endif
#if (ESC_AL_EVENTMASK_OFFSET != 0x0204)
#error "ESC_AL_EVENTMASK_OFFSET mismatch (expected 0x0204)"
#endif

#define PDI_TIMEOUT              100000UL

/* AL Event 影子寄存器（读 0x220 同时清除 ESC 中断源） */
static UINT16 s_alEvent;

/* EXTI3 入口时间戳（TIM2 96 MHz），用于 ISR 延迟统计 */
static UINT32 s_isrEntryTick;
/* 统计口径：PDI_Isr 每次进入会调用 HW_GetALEventRegister_Isr() 两次
 * （开头一次、循环超限检查一次），旧实现两次都记进同一份延迟直方图，
 * 第二次混入了 PDO 映射耗时，使 avg/max 被系统性放大。这里按“每次中断
 * 只记第一次 AL Event 读”的口径统计，其余调用不再计入。 */
static UINT8  s_isrLatArmed;

/* 接口链路诊断计数（只读统计） */
static HW_DIAG s_hwDiag;

static void DiagNoteAlEvent(UINT16 alEvent)
{
    if (s_hwDiag.FirstEventTick == 0U)
    {
        s_hwDiag.FirstEventTick = BSP_Get1msTick();
    }
    if ((alEvent & 0x0400U) != 0U)
    {
        s_hwDiag.OutEventCount++;
        s_hwDiag.LastOutTick = BSP_Get1msTick();
        if (s_hwDiag.FirstOutTick == 0U)
        {
            s_hwDiag.FirstOutTick = BSP_Get1msTick();
        }
    }
    if ((alEvent & 0x0800U) != 0U)
    {
        s_hwDiag.InEventCount++;
    }
    s_hwDiag.AlEventLast = alEvent;
    s_hwDiag.AlEventReadCount++;
}

HW_DIAG *HW_GetDiag(void)
{
    return &s_hwDiag;
}

/* SSC 判定 PD 看门狗超时（0x001B）时调用，记录报错时刻与距最近一次
 * PROCESS_OUTPUT_EVENT 的毫秒数，用于确定 ESC 实际看门狗周期 */
void HW_NoteWdError(UINT16 status)
{
    s_hwDiag.WdErrCount++;
    s_hwDiag.WdErrStatus = status;
    s_hwDiag.WdErrSinceOut = BSP_Get1msTick() - s_hwDiag.LastOutTick;
}

/*-----------------------------------------------------------------------------------------
------    DC / SYNC 边沿时间戳统计
-----------------------------------------------------------------------------------------*/
/* TIM2 = 96 MHz 16 位自由计数（682.67 us 回绕），TIM3 = 2 MHz 16 位（32.767 ms 回绕）。
 * 用 TIM3 的粗计数解 TIM2 回绕，得到 10.4 ns 分辨率的跨周期差值。 */
#define SYNC_HIST_BUCKETS       8U
#define SYNC_PERIOD_RING        128U
#define SYNC_GAP_INVALID_T3     65000U   /* >32.5 ms：视为断流 */

static UINT32 s_sync0LastT2;
static UINT32 s_sync0LastT3;
static UINT32 s_sync1LastT2;
static UINT32 s_sync1LastT3;
static UINT8  s_sync0HaveLast;
static UINT8  s_sync1HaveLast;
static UINT32 s_sync0PeriodRing[SYNC_PERIOD_RING];
static UINT16 s_sync0RingHead;
static UINT16 s_sync0RingCount;
static UINT32 s_sync1PeriodRing[SYNC_PERIOD_RING];
static UINT16 s_sync1RingHead;
static UINT16 s_sync1RingCount;

static UINT32 SyncTickDelta(UINT32 t2a, UINT32 t3a, UINT32 t2b, UINT32 t3b, UINT16 *gap3)
{
    INT32 d2 = (INT32)(UINT16)(t2b - t2a);   /* 96 MHz ticks，可能已回绕 */
    INT32 d3 = (INT32)(UINT16)(t3b - t3a);   /* 2 MHz ticks */
    INT32 est = d3 * 48;                     /* 2 MHz -> 96 MHz */
    INT32 n = est - d2;
    INT32 wraps;

    if (n >= 0)
    {
        wraps = (n + 32768) / 65536;
    }
    else
    {
        wraps = -(((-n) + 32768) / 65536);
    }

    if (gap3 != 0)
    {
        *gap3 = (UINT16)d3;
    }
    n = d2 + (wraps * 65536);
    return (UINT32)((n > 0) ? n : 0);
}

static void NoteSync0Edge(UINT32 t2, UINT32 t3)
{
    UINT16 gap3 = 0U;
    UINT32 dt;

    s_hwDiag.Sync0EdgeCount++;

    if (s_sync0HaveLast != 0U)
    {
        dt = SyncTickDelta(s_sync0LastT2, s_sync0LastT3, t2, t3, &gap3);
        if (gap3 <= SYNC_GAP_INVALID_T3)
        {
            if ((s_hwDiag.Sync0PeriodMinTicks == 0U) ||
                (dt < s_hwDiag.Sync0PeriodMinTicks))
            {
                s_hwDiag.Sync0PeriodMinTicks = dt;
            }
            if (dt > s_hwDiag.Sync0PeriodMaxTicks)
            {
                s_hwDiag.Sync0PeriodMaxTicks = dt;
            }
            s_hwDiag.Sync0PeriodSumTicks += dt;

            s_sync0PeriodRing[s_sync0RingHead] = dt;
            s_sync0RingHead = (UINT16)((s_sync0RingHead + 1U) % SYNC_PERIOD_RING);
            if (s_sync0RingCount < SYNC_PERIOD_RING)
            {
                s_sync0RingCount++;
            }
        }
    }

    s_sync0LastT2 = t2;
    s_sync0LastT3 = t3;
    s_sync0HaveLast = 1U;
}

static void NoteSync1Edge(UINT32 t2, UINT32 t3)
{
    UINT16 gap3 = 0U;
    UINT32 dt;

    s_hwDiag.Sync1EdgeCount++;

    if (s_sync1HaveLast != 0U)
    {
        dt = SyncTickDelta(s_sync1LastT2, s_sync1LastT3, t2, t3, &gap3);
        if (gap3 <= SYNC_GAP_INVALID_T3)
        {
            if ((s_hwDiag.Sync1PeriodMinTicks == 0U) ||
                (dt < s_hwDiag.Sync1PeriodMinTicks))
            {
                s_hwDiag.Sync1PeriodMinTicks = dt;
            }
            if (dt > s_hwDiag.Sync1PeriodMaxTicks)
            {
                s_hwDiag.Sync1PeriodMaxTicks = dt;
            }
            s_hwDiag.Sync1PeriodSumTicks += dt;
            s_hwDiag.Sync1LastPeriodTicks = dt;

            s_sync1PeriodRing[s_sync1RingHead] = dt;
            s_sync1RingHead = (UINT16)((s_sync1RingHead + 1U) % SYNC_PERIOD_RING);
            if (s_sync1RingCount < SYNC_PERIOD_RING)
            {
                s_sync1RingCount++;
            }
        }
        else
        {
            s_hwDiag.Sync1InvalidCount++;
        }
    }
    else
    {
        s_hwDiag.Sync1InvalidCount++;
    }

    /* SYNC1 边沿相对最近一次 SYNC0 边沿的相位（DC 下应等于 0x09A4） */
    if (s_sync0HaveLast != 0U)
    {
        UINT16 gapPhase = 0U;
        UINT32 ph = SyncTickDelta(s_sync0LastT2, s_sync0LastT3, t2, t3, &gapPhase);

        if (gapPhase <= 40000U)   /* <20 ms，避免相位跨越多个周期后误解回绕 */
        {
            if ((s_hwDiag.Sync1PhaseMinTicks == 0U) ||
                (ph < s_hwDiag.Sync1PhaseMinTicks))
            {
                s_hwDiag.Sync1PhaseMinTicks = ph;
            }
            if (ph > s_hwDiag.Sync1PhaseMaxTicks)
            {
                s_hwDiag.Sync1PhaseMaxTicks = ph;
            }
            s_hwDiag.Sync1PhaseSumTicks += (INT32)ph;
            s_hwDiag.Sync1PhaseN++;
            s_hwDiag.Sync1LastPhaseTicks = ph;
        }
    }

    s_sync1LastT2 = t2;
    s_sync1LastT3 = t3;
    s_sync1HaveLast = 1U;
}

void HW_ResetSyncStats(void)
{
    UINT16 i;

    s_hwDiag.Sync0EdgeCount = 0U;
    s_hwDiag.Sync1EdgeCount = 0U;
    s_hwDiag.Sync0PeriodMinTicks = 0U;
    s_hwDiag.Sync0PeriodMaxTicks = 0U;
    s_hwDiag.Sync0PeriodSumTicks = 0U;
    s_hwDiag.Sync1PeriodMinTicks = 0U;
    s_hwDiag.Sync1PeriodMaxTicks = 0U;
    s_hwDiag.Sync1PeriodSumTicks = 0U;
    s_hwDiag.Sync1PhaseMinTicks = 0U;
    s_hwDiag.Sync1PhaseMaxTicks = 0U;
    s_hwDiag.Sync1PhaseSumTicks = 0;
    s_hwDiag.Sync1LastPhaseTicks = 0U;
    s_hwDiag.Sync1LastPeriodTicks = 0U;
    s_hwDiag.Sync1InvalidCount = 0U;
    s_hwDiag.Sync1PhaseN = 0U;

    for (i = 0U; i < SYNC_PERIOD_RING; i++)
    {
        s_sync0PeriodRing[i] = 0U;
        s_sync1PeriodRing[i] = 0U;
    }
    s_sync0RingHead = 0U;
    s_sync0RingCount = 0U;
    s_sync1RingHead = 0U;
    s_sync1RingCount = 0U;
    s_sync0HaveLast = 0U;
    s_sync1HaveLast = 0U;
    s_sync0LastT2 = 0U;
    s_sync0LastT3 = 0U;
    s_sync1LastT2 = 0U;
    s_sync1LastT3 = 0U;
}

static UINT32 SyncRingMean(const UINT32 *ring, UINT16 count)
{
    UINT16 i;
    UINT32 sum = 0U;

    if (count == 0U)
    {
        return 0U;
    }
    for (i = 0U; i < count; i++)
    {
        sum += ring[i];
    }
    return sum / count;
}

UINT32 HW_GetSync0AvgTicks(void)
{
    return SyncRingMean(s_sync0PeriodRing, s_sync0RingCount);
}

UINT32 HW_GetSync1AvgTicks(void)
{
    return SyncRingMean(s_sync1PeriodRing, s_sync1RingCount);
}

/* 周期相对均值的偏差直方图；桶边界按 96 MHz tick 折算：
 * 25/50/100/200/500/1000/2000 ns -> 2/5/10/19/48/96/192 ticks */
void HW_GetSync1Hist(UINT16 hist[SYNC_HIST_BUCKETS])
{
    UINT16 i;
    UINT32 sum = 0U;
    UINT32 mean;

    for (i = 0U; i < SYNC_HIST_BUCKETS; i++)
    {
        hist[i] = 0U;
    }
    if (s_sync1RingCount == 0U)
    {
        return;
    }

    for (i = 0U; i < s_sync1RingCount; i++)
    {
        sum += s_sync1PeriodRing[i];
    }
    mean = sum / s_sync1RingCount;

    for (i = 0U; i < s_sync1RingCount; i++)
    {
        UINT32 v = s_sync1PeriodRing[i];
        UINT32 dev = (v > mean) ? (v - mean) : (mean - v);
        UINT8 b = 0U;

        if (dev > 192U)      { b = 7U; }
        else if (dev > 96U)  { b = 6U; }
        else if (dev > 48U)  { b = 5U; }
        else if (dev > 19U)  { b = 4U; }
        else if (dev > 10U)  { b = 3U; }
        else if (dev > 5U)   { b = 2U; }
        else if (dev > 2U)   { b = 1U; }
        hist[b]++;
    }
}

static void GetInterruptRegister(void)
{
    HW_EscRead((MEM_ADDR *)&s_alEvent, ESC_AL_EVENT_OFFSET, 2);
    DiagNoteAlEvent(s_alEvent);
}

static void ISR_GetInterruptRegister(void)
{
    UINT32 dt;
    UINT8 bucket = 0U;
    UINT32 threshold = BOARD_TICKS_PER_US;   /* 1 us（随主频变化） */

    s_hwDiag.IrqLevelIsr = BSP_GetIrqLevel();
    HW_EscReadIsr((MEM_ADDR *)&s_alEvent, ESC_AL_EVENT_OFFSET, 2);

    /* 入口 -> 读走 AL Event；16 位计数器按 65536 取模。
     * 只统计当前中断里的第一次 AL Event 读；PDI_Isr 内部的重复读不计入
     * （否则第二次读会把 PDO 映射耗时算成“中断延迟”）。 */
    if (s_isrLatArmed != 0U)
    {
        s_isrLatArmed = 0U;
        dt = (uint16_t)(TIM2->CNT - s_isrEntryTick);
        if (s_hwDiag.IsrLatN == 0U)
        {
            s_hwDiag.IsrLatMinTicks = dt;
        }
        else if (dt < s_hwDiag.IsrLatMinTicks)
        {
            s_hwDiag.IsrLatMinTicks = dt;
        }
        if (dt > s_hwDiag.IsrLatMaxTicks)
        {
            s_hwDiag.IsrLatMaxTicks = dt;
        }
        s_hwDiag.IsrLatSumTicks += dt;
        s_hwDiag.IsrLatN++;

        while ((bucket < 7U) && (dt > threshold))
        {
            bucket++;
            threshold <<= 1;
        }
        s_hwDiag.IsrLatHist[bucket]++;
    }

    DiagNoteAlEvent(s_alEvent);
}

/////////////////////////////////////////////////////////////////////////////////////////
/**
 \return     0 if initialization was successful

 \brief    Initialize the PDI and the host controller (CH32V307).
*////////////////////////////////////////////////////////////////////////////////////////
UINT8 HW_Init(void)
{
    UINT32 data;
    UINT32 retry;
    UINT16 maskVal = 0x0100U;
    UINT8 result = 0;

    /* BSP：RCC/GPIO/SPI2/EXTI/TIM3/TIM4/Q_RESET/LED，并完成 E252 上电自检 */
    BSP_Init();

    if (BSP_GetE252Info()->result != BSP_E252_SELFTEST_OK)
    {
        BSP_SetErrorLed(1);
        return 1;
    }

    /* AL Event Mask 写读回环校验 */
    retry = 1000UL;
    do
    {
        HW_EscWriteWord(maskVal, ESC_AL_EVENTMASK_OFFSET);
        data = 0U;
        HW_EscReadWord(data, ESC_AL_EVENTMASK_OFFSET);
        retry--;
    } while ((data != 0x0100U) && (retry != 0UL));

    if (data != 0x0100U)
    {
        BSP_SetErrorLed(1);
        return 2;
    }

    /* DPRAM 读写回环校验（0x1F00 属 ESC 内部测试区） */
    retry = 1000UL;
    do
    {
        data = 0x12345678UL;
        HW_EscWriteDWord(data, 0x1F00U);
        data = 0U;
        HW_EscReadDWord(data, 0x1F00U);
        retry--;
    } while ((data != 0x12345678UL) && (retry != 0UL));

    if (data != 0x12345678UL)
    {
        BSP_SetErrorLed(1);
        return 3;
    }

    /* IRQ 配置：IRQ 使能 + 推挽输出（与 DSP 参考一致：0x54 <- 0x00000101） */
    PDI_PortWrite32(E252_REG_IRQ_CFG, 0x00000101UL);
    /* 中断使能：PDI 中断（与 DSP 参考一致：0x5C <- 0x00000001） */
    PDI_PortWrite32(E252_REG_INT_EN, 0x00000001UL);
    (void)PDI_PortRead32(E252_REG_INT_STS);

    /* 初始关闭 AL Event Mask，由状态机在 PREOP->SAFEOP 时设置 */
    data = 0U;
    HW_EscWriteDWord(data, ESC_AL_EVENTMASK_OFFSET);

    s_alEvent = 0U;

    /* EXTI3 不在 HW_Init 里使能：上电后 ESC 的 IRQ 可能已为低电平（AL Event 待读），
       此时若立刻开中断，CH32 EXTI 挂起位会被持续置位形成风暴（见 main.c FreeRun 说明）。
       EXTI3 由 main.c 在 FreeRun -> SM 模式切换时再打开 */
    NVIC_DisableIRQ(EXTI3_IRQn);
    NVIC_EnableIRQ(EXTI4_IRQn);
    NVIC_EnableIRQ(EXTI9_5_IRQn);

    return result;
}

/////////////////////////////////////////////////////////////////////////////////////////
/**
 \brief    Release hardware resources if the sample application stops.
*////////////////////////////////////////////////////////////////////////////////////////
void HW_Release(void)
{
    TIM_Cmd(TIM3, DISABLE);
    TIM_Cmd(TIM4, DISABLE);
    SPI_Cmd(E252_SPI, DISABLE);
    NVIC_DisableIRQ(EXTI3_IRQn);
    NVIC_DisableIRQ(EXTI4_IRQn);
    NVIC_DisableIRQ(EXTI9_5_IRQn);
}

/////////////////////////////////////////////////////////////////////////////////////////
/**
 \return    first two Bytes of AL Event register (0x220)

 \brief    Get the current content of the AL Event register.
*////////////////////////////////////////////////////////////////////////////////////////
UINT16 HW_GetALEventRegister(void)
{
    GetInterruptRegister();
    return s_alEvent;
}

#if INTERRUPTS_SUPPORTED
UINT16 HW_GetALEventRegister_Isr(void)
{
    ISR_GetInterruptRegister();
    return s_alEvent;
}
#endif

/* 翻转 IRQ_CFG 极性位，观察空闲电平是否反转后写回。
 * 只写易失寄存器 0x0054，不涉及 EEPROM；写回原值后清 EXTI 挂起位再开中断。 */
void HW_IrqPolarityProbe(UINT8 *levelNormal, UINT8 *levelInverted, UINT32 *cfgNormal)
{
    UINT32 cfg;
    UINT32 t0;

    cfg = PDI_PortRead32(E252_REG_IRQ_CFG);
    *cfgNormal = cfg;

    DISABLE_ESC_INT();
    /* IRQ_POL 是 IRQ_CFG 的 bit4（SYS-E252 手册 §9.3.1）：bit1 是 IRQ_CLK_SELECT，
     * 置 1 会把晶振时钟输出到 IRQ 引脚，不能用于极性检查 */
    PDI_PortWrite32(E252_REG_IRQ_CFG, cfg ^ 0x00000010UL);
    t0 = BSP_Get1msTick();
    while ((UINT32)(BSP_Get1msTick() - t0) < 2U)
    {
    }
    *levelInverted = BSP_GetIrqLevel();

    PDI_PortWrite32(E252_REG_IRQ_CFG, cfg);
    t0 = BSP_Get1msTick();
    while ((UINT32)(BSP_Get1msTick() - t0) < 2U)
    {
    }
    *levelNormal = BSP_GetIrqLevel();

    EXTI_ClearITPendingBit(EXTI_Line3);
    ENABLE_ESC_INT();
}

/* 诊断：在 OP 帧流中翻转 IRQ_CFG 极性位，检查 PA3 的边沿路径是否随之改变。
 * 正常极性（低有效）：事件把引脚拉低 → 下降沿 → EXT3 计数增加。
 * 反相极性（高有效）：同样帧流下事件把引脚拉高 → 无下降沿 → ΔEXT3≈0。
 * 恢复后下降沿立即恢复。全程只写易失寄存器 0x0054，不涉及 EEPROM。 */
void HW_IrqEdgeProbe(UINT32 *edgesInv, UINT32 *edgesNormal,
                     UINT8 *levelInv, UINT8 *levelNormal)
{
    UINT32 cfg;
    UINT32 baseEdges;
    UINT32 t0;

    cfg = PDI_PortRead32(E252_REG_IRQ_CFG);

    /* 反相：空闲电平应为低 */
    /* IRQ_POL 是 IRQ_CFG 的 bit4（SYS-E252 手册 §9.3.1）：bit1 是 IRQ_CLK_SELECT，
     * 置 1 会把晶振时钟输出到 IRQ 引脚，不能用于极性检查 */
    PDI_PortWrite32(E252_REG_IRQ_CFG, cfg ^ 0x00000010UL);
    t0 = BSP_Get1msTick();
    while ((UINT32)(BSP_Get1msTick() - t0) < 2U)
    {
    }
    *levelInv = BSP_GetIrqLevel();
    EXTI_ClearITPendingBit(EXTI_Line3);
    baseEdges = s_hwDiag.Exti3Count;
    t0 = BSP_Get1msTick();
    while ((UINT32)(BSP_Get1msTick() - t0) < 15U)
    {
    }
    *edgesInv = s_hwDiag.Exti3Count - baseEdges;

    /* 恢复：空闲电平应为高 */
    PDI_PortWrite32(E252_REG_IRQ_CFG, cfg);
    t0 = BSP_Get1msTick();
    while ((UINT32)(BSP_Get1msTick() - t0) < 2U)
    {
    }
    *levelNormal = BSP_GetIrqLevel();
    EXTI_ClearITPendingBit(EXTI_Line3);
    baseEdges = s_hwDiag.Exti3Count;
    t0 = BSP_Get1msTick();
    while ((UINT32)(BSP_Get1msTick() - t0) < 15U)
    {
    }
    *edgesNormal = s_hwDiag.Exti3Count - baseEdges;

    /* 反相窗口内积存的事件没有下降沿可触发，补一次软件中断排空 */
    EXTI_GenerateSWInterrupt(EXTI_Line3);
}

#if UC_SET_ECAT_LED
void HW_SetLed(UINT8 RunLed, UINT8 ErrLed)
{
    BSP_ApplyLeds((UINT8)(RunLed & 0x01U));
    BSP_SetErrorLed((UINT8)(ErrLed & 0x01U));
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////
/**
 \brief    Read data from ESC memory.
*////////////////////////////////////////////////////////////////////////////////////////
void HW_EscRead(MEM_ADDR *pData, UINT16 Address, UINT16 Len)
{
    UINT32 lock = E252_PdiLock();
    (void)PDI_Direct_EscRead(pData, Address, Len, PDI_TIMEOUT);
    E252_PdiUnlock(lock);
}

void HW_EscReadIsr(MEM_ADDR *pData, UINT16 Address, UINT16 Len)
{
    UINT32 lock = E252_PdiLock();
    (void)PDI_Direct_EscRead(pData, Address, Len, PDI_TIMEOUT);
    E252_PdiUnlock(lock);
}

/////////////////////////////////////////////////////////////////////////////////////////
/**
 \brief    Write data to ESC memory.
*////////////////////////////////////////////////////////////////////////////////////////
void HW_EscWrite(MEM_ADDR *pData, UINT16 Address, UINT16 Len)
{
    UINT32 lock = E252_PdiLock();
    (void)PDI_Direct_EscWrite(pData, Address, Len, PDI_TIMEOUT);
    E252_PdiUnlock(lock);
}

void HW_EscWriteIsr(MEM_ADDR *pData, UINT16 Address, UINT16 Len)
{
    UINT32 lock = E252_PdiLock();
    (void)PDI_Direct_EscWrite(pData, Address, Len, PDI_TIMEOUT);
    E252_PdiUnlock(lock);
}

/*-----------------------------------------------------------------------------------------
------    Interrupt handlers (vector names must match startup_ch32v30x_D8C.S)
-----------------------------------------------------------------------------------------*/

/* 与官方例程（TIM_INT/ch32v30x_it.c）一致：QingKe V4 硬件中断入口/返回必须声明
 * WCH-Interrupt-fast，否则普通函数以 ret 返回，PFIC active 位不清、MIE 不恢复，
 * 中断只会被投递一次（TIM4 只进一次、EXTI3 一次后不再响应）。
 *
 * 启动代码把 mstatus.MPP 置 0，若 mret 直接使用该值返回，线程态会落到用户态，
 * 之后线程态访问 mstatus 等机器 CSR 会报非法指令（此时 mepc 指向
 * csrr mstatus，mcause=2、mtval=0x300024f3）。ISR 在返回前显式置 MPP=3。 */
#define WCH_ISR_RETURN_MACHINE() \
    __asm volatile ("li t0, 0x1800\n\tcsrs mstatus, t0" ::: "t0", "memory")

void EXTI3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void EXTI4_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void EXTI15_10_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM4_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* SSC 在 ecatappl.c 中定义：TRUE = SM 同步模式（由中断处理过程数据），
 * FALSE = FreeRun（由 MainLoop 轮询 0x0220）。两条路径不能同时消费 AL Event。 */
extern BOOL bEscIntEnabled;

/* E252 IRQ -> PA3 -> EXTI3
 *
 * 退出顺序按 SYS-E252 数据手册：AL Event 请求寄存器（0x0220）的位是只读镜像，
 * 只有先清掉各自的源（SM 事件在 PDI 读走缓冲区首字后清除）才会归零；INT_STS
 * （0x0058）bit0 反映同一事件，也必须等源清后才为 0。CH32 的 EXTI3 在 PA3 仍为
 * 有效电平（低）时写入挂起位清不掉——在入口清挂起位无效，于是每次
 * mret 都被立即重新投递，可放大到 10~13.6 次/帧并把主循环饿死。
 * 正确顺序：PDI_Isr 清源 -> 查 INT_STS（清 bit30 READY 锁存）-> 等 PA3 回空闲高
 * -> 再清挂起位退出。 */
void EXTI3_IRQHandler(void)
{
    WCH_ISR_RETURN_MACHINE();
    s_isrEntryTick = TIM2->CNT;
    s_isrLatArmed = 1U;      /* 当前中断只记第一次 AL Event 读的延迟 */
    s_hwDiag.Exti3Count++;

    if (EXTI_GetITStatus(EXTI_Line3) == RESET)
    {
        /* 进入中断但挂起位已清除：IRQ 为电平信号、边沿可能被合并或丢失 */
        s_hwDiag.Exti3NoFlag++;
        return;
    }

    if (bEscIntEnabled == FALSE)
    {
        /* FreeRun：不读 0x0220，把事件留给 MainLoop 轮询（中断抢读会导致
           PROCESS_OUTPUT_EVENT 永远到不了轮询路径，
           bEcatFirstOutputsReceived 恒为 0 → SAFEOP→OP 超时 0x001B）
           该模式下 EXTI3 在 NVIC 中处于屏蔽状态，不会因 IRQ 保持低电平而风暴 */
        s_hwDiag.Exti3PollModeCount++;
        EXTI_ClearITPendingBit(EXTI_Line3);
        return;
    }

    s_hwDiag.PdiIsrCount++;
    PDI_Isr();

    /* 手册 §9.3.2：INT_STS 只有在中断被处理且其源已清除后才能清零。
     * bit0 为只读镜像（这里读回值应已为 0）；bit30 READY 为上电锁存，写 1 清。 */
    {
        UINT32 sts = PDI_PortRead32(E252_REG_INT_STS);

        s_hwDiag.IntStsAtExit = sts;
        if ((sts & E252_INT_STS_READY) != 0U)
        {
            PDI_PortWrite32(E252_REG_INT_STS, E252_INT_STS_READY); /* R/WC：写 1 清除 */
        }
        if ((sts & E252_INT_STS_ECAT) != 0U)
        {
            s_hwDiag.IntStsEcatStuck++;
        }
    }

    /* 等 PA3 回到空闲高（上限 10 us，防源未清时死等），再清挂起位；
     * 引脚仍为低时清挂起位会被立即重装，退出即重投。 */
    {
        UINT32 t0 = TIM2->CNT;
        UINT32 waited = 0U;

        while (BSP_GetIrqLevel() == 0U)
        {
            waited = (UINT16)(TIM2->CNT - t0);
            if (waited > E252_IRQ_DEASSERT_TICKS)
            {
                break;
            }
        }
        if (waited > s_hwDiag.IrqDeassertTicks)
        {
            s_hwDiag.IrqDeassertTicks = waited;
        }
        if (BSP_GetIrqLevel() == 0U)
        {
            s_hwDiag.IrqLowAtExit++;
        }
    }

    EXTI_ClearITPendingBit(EXTI_Line3);
}

/* E252 SYNC0 -> PA4 -> EXTI4 */
void EXTI4_IRQHandler(void)
{
    UINT32 t2;
    UINT32 t3;

    WCH_ISR_RETURN_MACHINE();
    if (EXTI_GetITStatus(EXTI_Line4) != RESET)
    {
        t2 = TIM2->CNT;
        t3 = TIM3->CNT;
        NoteSync0Edge(t2, t3);
        EXTI_ClearITPendingBit(EXTI_Line4);
        Sync0_Isr();
    }
}

/* E252 SYNC1 -> PB11 -> EXTI11（EXTI15_10 向量） */
void EXTI15_10_IRQHandler(void)
{
    UINT32 t2;
    UINT32 t3;

    WCH_ISR_RETURN_MACHINE();
    if (EXTI_GetITStatus(EXTI_Line11) != RESET)
    {
        t2 = TIM2->CNT;
        t3 = TIM3->CNT;
        NoteSync1Edge(t2, t3);
        EXTI_ClearITPendingBit(EXTI_Line11);
        Sync1_Isr();
    }
}

/* 1 ms EtherCAT watchdog tick */
static UINT32 s_tim4LastT3;

void TIM4_IRQHandler(void)
{
    WCH_ISR_RETURN_MACHINE();
    if (TIM_GetITStatus(TIM4, TIM_IT_Update) != RESET)
    {
        /* TIM4 实际间隔（TIM3 2 MHz，16 位，0.5 us/tick）：用于识别“tick 变慢”
         * （中断被长临界区延迟时，软件 1 ms 会大于真实 1 ms） */
        UINT32 t3 = TIM3->CNT;
        UINT32 dt3 = (UINT32)(UINT16)(t3 - s_tim4LastT3);

        s_tim4LastT3 = t3;
        if ((dt3 != 0U) && (dt3 < 30000U))   /* <15 ms 才计入，避免首次/断流拉偏 */
        {
            if ((s_hwDiag.Tim4DtN == 0U) || (dt3 < s_hwDiag.Tim4DtMinTicks))
            {
                s_hwDiag.Tim4DtMinTicks = dt3;
            }
            if (dt3 > s_hwDiag.Tim4DtMaxTicks)
            {
                s_hwDiag.Tim4DtMaxTicks = dt3;
            }
            s_hwDiag.Tim4DtSumTicks += dt3;
            s_hwDiag.Tim4DtN++;
        }

        TIM_ClearITPendingBit(TIM4, TIM_IT_Update);
        BSP_Inc1msTick();
        BSP_KeyTick();
        ECAT_CheckTimer();
    }
}

#endif /* EL9800_HW */
