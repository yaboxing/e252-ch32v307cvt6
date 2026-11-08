/**
 * @file    main.c
 * @brief   CH32V307 EtherCAT 从站入口
 *
 * 初始化顺序：HW_Init（BSP + E252 自检 + PDI/IRQ 配置）
 *             -> MainInit（SSC 栈与对象字典）
 *             -> MainLoop（循环处理）
 *
 * 调试串口：USART1（PA9/PA10，115200 8N1），printf 经 _write 重定向。
 */

#include <stdio.h>
#include "ecat_def.h"
#include "Ethercat/Inc/ecatappl.h"
#include "Ethercat/Inc/applInterface.h"
#include "Ethercat/port/el9800hw.h"
#include "hal/e252_spi.h"
#include "hal/lan9252_pdi_access.h"
#include "bsp/ch32v307_bsp.h"
#include "bsp/board_config.h"   /* BOARD_TICKS_PER_US */
#include "debug.h"
#include "ch32v30x_rcc.h"
#include "system_ch32v30x.h"

/* SSC 在 ecatslv.c / ecatappl.c 中定义，头文件未提供 extern 声明 */
extern UINT8 nAlStatus;
extern BOOL bEcatFirstOutputsReceived;
extern UINT16 nPdOutputSize;
extern UINT16 nPdInputSize;
extern UINT16 u16ALEventMask;
extern BOOL bEscIntEnabled;
extern UINT16 EcatWdValue;   /* SSC 计算的 PD 看门狗(ms)，0=未启用 */

/* 接口链路诊断快照（只读） */
typedef struct
{
    UINT16 alStatus;       /* 0x0130 */
    UINT16 alStatusCode;   /* 0x0134 */
    UINT16 alControl;      /* 0x0120 */
    UINT16 dlStatus;       /* 0x0110 */
    UINT16 pdiControl;     /* 0x0140 */
    UINT32 alEventMask;    /* 0x0204 */
    UINT16 wdDivider;      /* 0x0400 */
    UINT16 wdTimePd;       /* 0x0420 */
    UINT16 wdStatusPd;     /* 0x0440 */
    UINT16 wdCounters;     /* 0x0442=过程数据计数(低字节) 0x0443=PDI 计数(高字节) */
    UINT32 intSts;         /* 0x0058 */
    UINT32 irqCfg;         /* 0x0054 */
    UINT16 pdOut[3];       /* SM2 输出缓冲区 0x1200，6 字节 */
    UINT16 pdIn[3];        /* SM3 输入缓冲区 0x1300，6 字节 */
    UINT16 sm[4][4];       /* SM0~SM3: [addr][len][ctrl/status][activate/pdi] */
} EscSnapshot;

static UINT32 s_pdiTestFail;   /* 每秒一次 0x1F00 回环自检失败次数 */
static UINT16 s_pdiTestSeq;

/* SM2 输出整帧采样统计：主站交替下发 5A../A5.. 时统计整帧匹配次数，
 * 作为“主站发的 → ESC SM2 → CH32 读到”的逐字节匹配判据 */
static UINT32 s_pdOutAMatch;      /* 5A 00 34 12 78 56 */
static UINT32 s_pdOutBMatch;      /* A5 00 34 12 78 56 */
static UINT32 s_pdOutOtherCount;
static UINT8  s_pdOutOtherBytes[6];
static UINT32 s_pdOutSamples;
static UINT32 s_pdOutSampleTick;
static UINT8  s_irqPulseDone;   /* OP 后只做一次 IRQ 脉宽测量 */
static UINT8  s_irqEdgeDone;    /* OP 后只做一次极性/边沿路径检查 */
static UINT8  s_opNfReset;      /* 进入 OP 时把上电残留的 Exti3NoFlag 归零，统计期从 0 计 */

/* el9800appl.c 在每个 PDI 周期保存实际写入 TxPDO(SM3) 的 6 字节。
 * SM3 为“ECAT 读 / PDI 写”的 3 缓冲区模式时，PDI 回读 SM3 地址区不返回发送中的数据
 * （PDI 回读恒为 0，写入后立即回读也为 0），因此 TxPDO 只能由应用侧影子副本观测。 */
extern UINT8 g_DiagPdInMap[6];

/* 每 2 ms 整帧采样一次；读到中间态（ESC 正在换 buffer）时归入 other */
static void SamplePdOutPattern(uint32_t now)
{
    UINT8 buf[6];
    UINT16 v;

    if ((uint32_t)(now - s_pdOutSampleTick) < 2U)
    {
        return;
    }
    s_pdOutSampleTick = now;

    HW_EscRead((MEM_ADDR *)buf, 0x1200U, 6U);
    v = (UINT16)(buf[0] | ((UINT16)buf[1] << 8));

    if ((v == 0x005AU) && (buf[2] == 0x34U) && (buf[3] == 0x12U) &&
        (buf[4] == 0x78U) && (buf[5] == 0x56U))
    {
        s_pdOutAMatch++;
    }
    else if ((v == 0x00A5U) && (buf[2] == 0x34U) && (buf[3] == 0x12U) &&
             (buf[4] == 0x78U) && (buf[5] == 0x56U))
    {
        s_pdOutBMatch++;
    }
    else
    {
        s_pdOutOtherCount++;
        for (v = 0U; v < 6U; v++)
        {
            s_pdOutOtherBytes[v] = buf[v];
        }
    }
    s_pdOutSamples++;
}

/* PDI 链路稳定性自检：0x1F00 为 ESC 内部测试区（HW_Init 已用过），不涉及 EEPROM */
static void PdiLoopbackTest(void)
{
    UINT32 wr;
    UINT32 rd;

    s_pdiTestSeq++;
    wr = ((s_pdiTestSeq & 0x0001U) != 0U) ? 0x0000A5A5UL : 0x00005A5AUL;
    HW_EscWriteDWord(wr, 0x1F00U);
    rd = 0U;
    HW_EscReadDWord(rd, 0x1F00U);
    if (rd != wr)
    {
        s_pdiTestFail++;
    }
}

static void EscSnapshotRead(EscSnapshot *snap)
{
    snap->alStatus     = 0U;
    snap->alStatusCode = 0U;
    snap->alControl    = 0U;
    snap->dlStatus     = 0U;
    snap->pdiControl   = 0U;
    snap->alEventMask  = 0U;
    snap->wdDivider    = 0U;
    snap->wdTimePd     = 0U;
    snap->wdStatusPd   = 0U;

    HW_EscReadWord(snap->alStatus, 0x0130U);
    HW_EscReadWord(snap->alStatusCode, 0x0134U);
    HW_EscReadWord(snap->alControl, 0x0120U);
    HW_EscReadWord(snap->dlStatus, 0x0110U);
    HW_EscReadWord(snap->pdiControl, 0x0140U);
    HW_EscReadDWord(snap->alEventMask, 0x0204U);
    HW_EscReadWord(snap->wdDivider, 0x0400U);
    HW_EscReadWord(snap->wdTimePd, 0x0420U);
    /* 读 0x0440 会清除 AL Event bit6（看门狗事件），协议栈亦在计时器中读取 */
    HW_EscReadWord(snap->wdStatusPd, 0x0440U);
    /* 0x0442/0x0443 为相邻两个字节寄存器：按 16 位整体读取后拆分，
       避免对奇数地址做单字节读（本 PDI 移植层按 16 位 MEM_ADDR 打包，奇数地址会触发
       RISC-V load-misaligned 异常，mcause=4） */
    HW_EscReadWord(snap->wdCounters, 0x0442U);

    /* 主站下发的 PDO 输出缓冲区（SM2）与本地上传的输入缓冲区（SM3）。
     * 必须一次 6 字节事务读完。分 3 次 2 字节读会在两次事务之间被 ESC 换
     * buffer（3 缓冲区模式下缓冲区始终是最新数据），从而读到跨帧的混合内容。 */
    {
        UINT8 tmp[6];
        UINT8 k;

        HW_EscRead((MEM_ADDR *)tmp, 0x1200U, 6U);
        for (k = 0U; k < 3U; k++)
        {
            snap->pdOut[k] = (UINT16)((UINT16)tmp[k * 2U] |
                                      ((UINT16)tmp[k * 2U + 1U] << 8));
        }
        HW_EscRead((MEM_ADDR *)tmp, 0x1300U, 6U);
        for (k = 0U; k < 3U; k++)
        {
            snap->pdIn[k] = (UINT16)((UINT16)tmp[k * 2U] |
                                     ((UINT16)tmp[k * 2U + 1U] << 8));
        }
    }

    snap->intSts = PDI_PortRead32(0x0058U);
    snap->irqCfg  = PDI_PortRead32(0x0054U);

    {
        UINT8 i;
        for (i = 0U; i < 4U; i++)
        {
            /* E252 的 SyncManager/SCU 寄存器位于 0x0800（esc.h: ESC_SYNCMAN_REG_OFFSET） */
            UINT16 base = (UINT16)(0x0800U + ((UINT16)i * 0x0008U));
            HW_EscReadWord(snap->sm[i][0], (UINT16)(base + 0U));
            HW_EscReadWord(snap->sm[i][1], (UINT16)(base + 2U));
            HW_EscReadWord(snap->sm[i][2], (UINT16)(base + 4U));
            HW_EscReadWord(snap->sm[i][3], (UINT16)(base + 6U));
        }
    }
}

static void PrintPdBytes(const char *tag, UINT16 addr, const UINT16 *w)
{
    UINT8 i;

    printf("[%s] 0x%04X=", tag, (unsigned)addr);
    for (i = 0U; i < 6U; i++)
    {
        unsigned int b = ((i & 1U) != 0U) ? (unsigned int)(w[i >> 1] >> 8)
                                          : (unsigned int)(w[i >> 1] & 0xFFU);
        printf(" %02X", b);
    }
    printf("\r\n");
}

/* 读取 64 位 ESC 寄存器（低 32 位在 addr，高 32 位在 addr+4） */
static void ReadEsc64(UINT16 addr, UINT32 *lo, UINT32 *hi)
{
    *lo = 0U;
    *hi = 0U;
    HW_EscReadDWord(*lo, addr);
    HW_EscReadDWord(*hi, (UINT16)(addr + 4U));
}

/* SYNC0(PA4)/SYNC1(PB11) 双引脚电平采样诊断：每 5 s 做一次约 1 ms 紧循环，
 * 统计样本数/低电平数/翻转次数/首个电平/低电平宽度。
 * TIM2 为 96 MHz、16 位自由计数（单圈约 682 µs），按 16 位增量累加计时；
 * 不使用 TIM4 的 1 ms tick（1 kHz 对 1 kHz 会相位锁定，看不到低电平）。
 * 这是“边沿中断之外”的独立观测路径：边沿计数说明“有跳变”，
 * 电平窗口说明“极性/宽度/占空比/未激活时是否静止”。 */
static void SampleSyncPins(void)
{
    static uint8_t s_callDiv = 0U;
    /* 窗口取 2 ms（2 个 SYNC0 周期）：最坏情况窗口起点晚于 SYNC0 下降沿近 1 ms
     * （等效于刚错过该周期），下一个 SYNC0 下降沿在 +1 ms，再叠加最大 750 µs
     * 偏移才出现 SYNC1 下降沿，合计 1.75 ms < 2 ms，保证每次窗口都能测到
     * 一对 SYNC0→SYNC1 下降沿。若窗口仅 1~1.2 ms，500/750 µs 段会漏采样
     * （500 µs 段可能出现两次 phn=0）。 */
    const uint32_t targetTicks = 2000UL * BOARD_TICKS_PER_US;
    const uint32_t periodTicks = 1000UL * BOARD_TICKS_PER_US;
    uint32_t prevTick = TIM2->CNT;
    uint32_t elapsed = 0UL;
    uint32_t n = 0UL;
    uint32_t low0 = 0UL;
    uint32_t low1 = 0UL;
    uint32_t chg0 = 0UL;
    uint32_t chg1 = 0UL;
    uint32_t guard = 0UL;
    uint32_t fall0 = 0UL;     /* 窗口内最近一次 SYNC0 下降沿位置（tick） */
    uint32_t fall1 = 0UL;     /* 窗口内最近一次 SYNC1 下降沿位置（tick） */
    uint32_t w0 = 0UL;        /* 首个完整 SYNC0 低电平宽度（tick） */
    uint32_t w1 = 0UL;        /* 首个完整 SYNC1 低电平宽度（tick） */
    uint32_t phTicks = 0UL;   /* SYNC1 下降沿 - 最近 SYNC0 下降沿（tick） */
    uint32_t phN = 0UL;
    uint8_t prev0;
    uint8_t prev1;
    uint8_t lv0;
    uint8_t lv1;
    uint8_t first0;
    uint8_t first1;

    /* 每 5 s 采样一次（1.2 ms 窗口），把对主循环的阻塞降到 0.02% */
    if (++s_callDiv < 5U)
    {
        return;
    }
    s_callDiv = 0U;

    first0 = (uint8_t)((GPIOA->INDR >> 4) & 1U);
    first1 = (uint8_t)((GPIOB->INDR >> 11) & 1U);
    prev0 = first0;
    prev1 = first1;
    lv0 = first0;
    lv1 = first1;

    /* guard 是硬性迭代上限：即使 TIM2 时基异常也保证 1~2 ms 内退出 */
    while ((elapsed < targetTicks) && (guard < 60000UL))
    {
        uint32_t tick = TIM2->CNT;

        lv0 = (uint8_t)((GPIOA->INDR >> 4) & 1U);
        lv1 = (uint8_t)((GPIOB->INDR >> 11) & 1U);
        elapsed += (uint32_t)(uint16_t)(tick - prevTick);
        prevTick = tick;

        n++;
        if (lv0 == 0U) { low0++; }
        if (lv1 == 0U) { low1++; }
        if (lv0 != prev0)
        {
            chg0++;
            if (lv0 == 0U)
            {
                fall0 = elapsed;   /* SYNC0 下降沿（有效边沿）位置 */
            }
            else if ((fall0 != 0UL) && (w0 == 0UL))
            {
                w0 = elapsed - fall0;   /* 首个完整低电平宽度 */
            }
            prev0 = lv0;
        }
        if (lv1 != prev1)
        {
            chg1++;
            if (lv1 == 0U)
            {
                fall1 = elapsed;
                /* 同一窗口内：SYNC1 下降沿相对最近一次 SYNC0 下降沿的延迟。
                 * E252 手册 §13.14.82：0x09A4 就是“SYNC1 相对 SYNC0 的间隔”，
                 * 因此这个引脚级延迟必须等于配置的偏移（250/500/750 µs）。 */
                if (fall0 != 0UL)
                {
                    uint32_t d = elapsed - fall0;
                    if (d < periodTicks)
                    {
                        phTicks = d;
                        phN++;
                    }
                }
            }
            else if ((fall1 != 0UL) && (w1 == 0UL))
            {
                w1 = elapsed - fall1;   /* 首个完整低电平宽度 */
            }
            prev1 = lv1;
        }
        guard++;
    }

    /* low_us 仍是“低电平样本占比 × 窗口时长”（窗口内可能含 1~2 个脉冲，只作粗值）；
     * w0us/w1us 是边沿到边沿测得的首个完整低电平宽度（精确到 TIM2 一个 tick）；
     * ph_us/phn 是同一窗口内 SYNC1 下降沿相对 SYNC0 下降沿的延迟（对应 0x09A4 的值）。 */
    printf("[SYNCPIN] win_us=%lu S0 n=%lu low=%lu chg=%lu low_us=%lu w0us=%lu lvl=%u | S1 n=%lu low=%lu chg=%lu low_us=%lu w1us=%lu lvl=%u | ph_us=%lu phn=%lu\r\n",
           (unsigned long)(elapsed / BOARD_TICKS_PER_US),
           (unsigned long)n, (unsigned long)low0, (unsigned long)chg0,
           (unsigned long)((n != 0U) ? ((low0 * (elapsed / BOARD_TICKS_PER_US)) / n) : 0U),
           (unsigned long)(w0 / BOARD_TICKS_PER_US),
           (unsigned)first0,
           (unsigned long)n, (unsigned long)low1, (unsigned long)chg1,
           (unsigned long)((n != 0U) ? ((low1 * (elapsed / BOARD_TICKS_PER_US)) / n) : 0U),
           (unsigned long)(w1 / BOARD_TICKS_PER_US),
           (unsigned)first1,
           (unsigned long)(phTicks / BOARD_TICKS_PER_US),
           (unsigned long)phN);
}

/* 紧凑 DC/SYNC 行：每 1 s 一行（约 130 字节），长时间抓取也不会因串口吞吐丢数据。
 * 详细诊断（[EVT]/[WD ]/[DC ]/[TICK] 等）每 5 s 输出一次。 */
static void PrintDcCompact(uint32_t now)
{
    HW_DIAG *d = HW_GetDiag();
    UINT32 n0 = d->Sync0EdgeCount;
    UINT32 n1 = d->Sync1EdgeCount;
    UINT32 a0 = (n0 > 1U) ? (d->Sync0PeriodSumTicks / (n0 - 1U)) : 0U;
    UINT32 a1 = (n1 > 1U) ? (d->Sync1PeriodSumTicks / (n1 - 1U)) : 0U;
    UINT32 ph = (d->Sync1PhaseN != 0U)
                ? ((UINT32)d->Sync1PhaseSumTicks / d->Sync1PhaseN) : 0U;

    SampleSyncPins();

    printf("[DCS ] t=%lu AL=0x%02X S0=%lu S1=%lu p0us=%lu/%lu/%lu p1us=%lu/%lu/%lu phus=%lu phlast=%lu bad=%lu\r\n",
           (unsigned long)now,
           (unsigned)nAlStatus,
           (unsigned long)n0, (unsigned long)n1,
           (unsigned long)(d->Sync0PeriodMinTicks / BOARD_TICKS_PER_US),
           (unsigned long)(a0 / BOARD_TICKS_PER_US),
           (unsigned long)(d->Sync0PeriodMaxTicks / BOARD_TICKS_PER_US),
           (unsigned long)(d->Sync1PeriodMinTicks / BOARD_TICKS_PER_US),
           (unsigned long)(a1 / BOARD_TICKS_PER_US),
           (unsigned long)(d->Sync1PeriodMaxTicks / BOARD_TICKS_PER_US),
           (unsigned long)(ph / BOARD_TICKS_PER_US),
           (unsigned long)(d->Sync1LastPhaseTicks / BOARD_TICKS_PER_US),
           (unsigned long)d->Sync1InvalidCount);
}

/* PDI 直读 DC 状态：只用合法 CSR 组合（size1@任意、size4@4 字节对齐），
 * 全部经 PDI_Direct_EscRead 封装（内含 32 位 busy 轮询 + 超时上限），
 * 读失败只把对应 ok 位置 0，不会阻塞主循环。
 * 0x0981 = 激活寄存器（bit1 SYNC0 / bit2 SYNC1），0x0984 = 激活状态，
 * 0x09A0 = SYNC0 周期(ns)，0x09A4 = SYNC1 相对 SYNC0 的间隔(ns)。 */
static void PrintDcRegsDirect(void)
{
    UINT16 act = 0U;
    UINT16 st = 0U;
    MEM_ADDR b0[2] = { 0U, 0U };
    MEM_ADDR b1[2] = { 0U, 0U };
    UINT32 cyc0;
    UINT32 off1;
    UINT8 ok = 0U;

    if (PDI_Direct_EscRead((MEM_ADDR *)&act, 0x0981U, 1U, 200U) == PDI_STATUS_OK) { ok |= 0x01U; }
    if (PDI_Direct_EscRead((MEM_ADDR *)&st, 0x0984U, 1U, 200U) == PDI_STATUS_OK) { ok |= 0x02U; }
    if (PDI_Direct_EscRead(b0, 0x09A0U, 4U, 200U) == PDI_STATUS_OK) { ok |= 0x04U; }
    if (PDI_Direct_EscRead(b1, 0x09A4U, 4U, 200U) == PDI_STATUS_OK) { ok |= 0x08U; }

    cyc0 = ((UINT32)(b0[0] & 0xFFFFU)) | (((UINT32)(b0[1] & 0xFFFFU)) << 16);
    off1 = ((UINT32)(b1[0] & 0xFFFFU)) | (((UINT32)(b1[1] & 0xFFFFU)) << 16);

    printf("[DCRD] ok=0x%02X 0981=0x%02X 0984=0x%02X 09A0=%lu 09A4=%lu csrTmo=%lu\r\n",
           (unsigned)ok, (unsigned)act, (unsigned)st,
           (unsigned long)cyc0, (unsigned long)off1,
           (unsigned long)PDI_GetCsrTimeoutCount());
}

static void PrintDiag(uint32_t now)
{
    HW_DIAG *diag = HW_GetDiag();
    EscSnapshot snap;
    int32_t sinceOut;

    diag->IrqLevelIdle = BSP_GetIrqLevel();
    EscSnapshotRead(&snap);
    PdiLoopbackTest();

    printf("[ECAT] t=%lu AL=0x%02X/CODE=0x%04X CTL=0x%02X DLSTS=0x%04X PDICTL=0x%04X\r\n",
           (unsigned long)now,
           (unsigned)nAlStatus,
           (unsigned)snap.alStatusCode,
           (unsigned)(snap.alControl & 0xFFU),
           (unsigned)snap.dlStatus,
           (unsigned)snap.pdiControl);

    printf("[EVT ] MASK=0x%08lX LAST=0x%04X EXT3=%lu(nf=%lu,fr=%lu) PDI=%lu OUT=%lu IN=%lu IRQ=%u/%u\r\n",
           (unsigned long)snap.alEventMask,
           (unsigned)diag->AlEventLast,
           (unsigned long)diag->Exti3Count,
           (unsigned long)diag->Exti3NoFlag,
           (unsigned long)diag->Exti3PollModeCount,
           (unsigned long)diag->PdiIsrCount,
           (unsigned long)diag->OutEventCount,
           (unsigned long)diag->InEventCount,
           (unsigned)diag->IrqLevelIsr,
           (unsigned)diag->IrqLevelIdle);

    printf("[LED ] mask=0x%02X ccr1=%u ccr2=%u sw=0x%02X\r\n",
           (unsigned)BSP_GetLedShadow(),
           (unsigned)TIM2->CH1CVR,
           (unsigned)TIM2->CH2CVR,
           (unsigned)BSP_ReadSwitches());

    /* 运行时 HSE 监测：HSEON=1 而 HSERDY=0 说明晶振已停振/起振失败。
     * 此时置兜底标志，后续任何 SAFEOP->OP 请求都会被 APPL_StartOutputHandler 拒绝。 */
    if ((RCC->CTLR & (RCC_HSEON | RCC_HSERDY)) == RCC_HSEON)
    {
        g_ClkDiagFallback = 1U;
    }
    printf("[CLK ] HSEON=%u HSERDY=%u PLLSRC=%u SWS=%u path=%lu fallback=%lu SYSCLK=%lu BRR=0x%04X\r\n",
           (unsigned)((RCC->CTLR & RCC_HSEON) ? 1U : 0U),
           (unsigned)((RCC->CTLR & RCC_HSERDY) ? 1U : 0U),
           (unsigned)((RCC->CFGR0 & RCC_PLLSRC) ? 1U : 0U),
           (unsigned)((RCC->CFGR0 & RCC_SWS) >> 2),
           (unsigned long)g_ClkDiagPath,
           (unsigned long)g_ClkDiagFallback,
           (unsigned long)SystemCoreClock,
           (unsigned)(USART1->BRR & 0xFFFFU));

    printf("[MODE] IntMode=%u FRAME=%u PDO=%u/%u EcatWd=%u WDSTAT=0x%04X WDc=%u/%u WDdiv/time=0x%04X/0x%04X INT_STS=0x%08lX IRQCFG=0x%08lX PDIloop=%lu csrTmo=%lu\r\n",
           (unsigned)bEscIntEnabled,
           (unsigned)bEcatFirstOutputsReceived,
           (unsigned)nPdOutputSize,
           (unsigned)nPdInputSize,
           (unsigned)EcatWdValue,
           (unsigned)snap.wdStatusPd,
           (unsigned)snap.wdCounters & 0xFFU, ((unsigned)snap.wdCounters >> 8) & 0xFFU,
           (unsigned)snap.wdDivider,
           (unsigned)snap.wdTimePd,
           (unsigned long)snap.intSts,
           (unsigned long)snap.irqCfg,
           (unsigned long)s_pdiTestFail,
           (unsigned long)PDI_GetCsrTimeoutCount());

    PrintDcRegsDirect();

    printf("[WD  ] WdErr=%lu SinceOut=%lums WdErrStat=0x%04X LastOutTick=%lu\r\n",
           (unsigned long)diag->WdErrCount,
           (unsigned long)diag->WdErrSinceOut,
           (unsigned)diag->WdErrStatus,
           (unsigned long)diag->LastOutTick);

    /* 每秒统计一次 ISR 入口 -> 读走 AL Event 的耗时与 PDI 临界区最长窗口 */
    {
        UINT32 n = diag->IsrLatN;
        UINT32 avgUs = (n != 0U) ? ((diag->IsrLatSumTicks / n) / BOARD_TICKS_PER_US) : 0UL;
        UINT32 lockUs = E252_GetPdiLockMaxTicks() / BOARD_TICKS_PER_US;
        UINT32 csrPollAvg;
        UINT32 csrPollMax;
        UINT32 segCmd;
        UINT32 segPoll;
        UINT32 segData;
        UINT32 segN;

        PDI_TakeCsrPollStats(&csrPollAvg, &csrPollMax);
        PDI_TakeCsrSegStats(&segCmd, &segPoll, &segData, &segN);

        printf("[IRQ ] EXT3=%lu nf=%lu fr=%lu PDI=%lu lvl=%u/%u lat_us=%lu/%lu/%lu n=%lu h=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu lock_us=%lu csrp=%lu/%lu sts=0x%08lX stuck=%lu low=%lu deas_us=%lu\r\n",
               (unsigned long)diag->Exti3Count,
               (unsigned long)diag->Exti3NoFlag,
               (unsigned long)diag->Exti3PollModeCount,
               (unsigned long)diag->PdiIsrCount,
               (unsigned)diag->IrqLevelIsr, (unsigned)diag->IrqLevelIdle,
               (unsigned long)(diag->IsrLatMinTicks / BOARD_TICKS_PER_US),
               (unsigned long)avgUs,
               (unsigned long)(diag->IsrLatMaxTicks / BOARD_TICKS_PER_US),
               (unsigned long)n,
               (unsigned long)diag->IsrLatHist[0], (unsigned long)diag->IsrLatHist[1],
               (unsigned long)diag->IsrLatHist[2], (unsigned long)diag->IsrLatHist[3],
               (unsigned long)diag->IsrLatHist[4], (unsigned long)diag->IsrLatHist[5],
               (unsigned long)diag->IsrLatHist[6], (unsigned long)diag->IsrLatHist[7],
               (unsigned long)lockUs,
               (unsigned long)csrPollAvg,
               (unsigned long)csrPollMax,
               (unsigned long)diag->IntStsAtExit,
               (unsigned long)diag->IntStsEcatStuck,
               (unsigned long)diag->IrqLowAtExit,
               (unsigned long)(diag->IrqDeassertTicks / BOARD_TICKS_PER_US));

        /* AL Event CSR 读的分段耗时 + DMA 裸吞吐 */
        {
            UINT32 r8  = E252_SPI_RawBench(8U);
            UINT32 r64 = E252_SPI_RawBench(64U);

            printf("[SEG ] al_n=%lu cmd_us=%lu poll_us=%lu data_us=%lu raw8_us=%lu raw64_us=%lu byte_ns=%lu dmaerr=%lu\r\n",
                   (unsigned long)segN,
                   (unsigned long)segCmd,
                   (unsigned long)segPoll,
                   (unsigned long)segData,
                   (unsigned long)(r8 / BOARD_TICKS_PER_US),
                   (unsigned long)(r64 / BOARD_TICKS_PER_US),
                   (unsigned long)((r64 * 1000UL) / (64UL * BOARD_TICKS_PER_US)),
                   (unsigned long)E252_SPI_GetDmaTimeouts());
        }

        diag->IsrLatN = 0U;
        diag->IsrLatSumTicks = 0U;
        diag->IsrLatMinTicks = 0U;
        diag->IsrLatMaxTicks = 0U;
        for (n = 0U; n < 8U; n++)
        {
            diag->IsrLatHist[n] = 0U;
        }
    }

    /* E252 分布式时钟寄存器快照（只读，不改变 DC 行为） */
    {
        UINT16 escCfg = 0U;
        UINT16 syncPdi = 0U;
        UINT16 pulse = 0U;
        UINT8  unitCtl = 0U;
        UINT8  dcAct = 0U;
        UINT16 dcStat = 0U;
        UINT16 speedDiff = 0U;
        UINT32 dcDiff = 0U;
        UINT32 cyc0 = 0U;
        UINT32 int1 = 0U;
        UINT32 dcCtrl = 0U;
        UINT32 spd = 0U;
        UINT32 stLo;
        UINT32 stHi;
        UINT32 ofLo;
        UINT32 ofHi;
        UINT32 s0Lo;
        UINT32 s0Hi;
        UINT32 s1Lo;
        UINT32 s1Hi;

        /* 0x0980~0x0983 处于同一 32 位 CSR 字：按 4 字节读后拆字节，
           避免对 0x0982 单独做 2 字节 CSR 读（本 PDI 路径返回 0）。 */
        HW_EscReadWord(escCfg, 0x0141U);
        HW_EscReadWord(syncPdi, 0x0151U);
        HW_EscReadDWord(dcCtrl, 0x0980U);
        unitCtl = (UINT8)(dcCtrl & 0xFFU);
        dcAct   = (UINT8)((dcCtrl >> 8) & 0xFFU);
        pulse   = (UINT16)((dcCtrl >> 16) & 0xFFFFU);   /* 0x0982~0x0983 */
        HW_EscReadWord(dcStat, 0x0984U);
        HW_EscReadDWord(spd, 0x0930U);
        speedDiff = (UINT16)(spd >> 16);
        HW_EscReadDWord(dcDiff, 0x092CU);
        HW_EscReadDWord(cyc0, 0x09A0U);
        HW_EscReadDWord(int1, 0x09A4U);
        ReadEsc64(0x0910U, &stLo, &stHi);
        ReadEsc64(0x0920U, &ofLo, &ofHi);
        ReadEsc64(0x0990U, &s0Lo, &s0Hi);
        ReadEsc64(0x0998U, &s1Lo, &s1Hi);

        printf("[DC  ] CFG141=0x%04X SYNC151=0x%04X PULSE982(lo)=%u ACT=%u UCTL=%u STAT984=0x%04X S0CYC=%lu S1INT=%lu DIFF=%ld SPDDIFF=%u\r\n",
               (unsigned)escCfg,
               (unsigned)syncPdi,
               (unsigned)pulse,
               (unsigned)dcAct,
               (unsigned)unitCtl,
               (unsigned)dcStat,
               (unsigned long)cyc0,
               (unsigned long)int1,
               (long)(INT32)dcDiff,
               (unsigned)speedDiff);
        printf("[DC0 ] SYSTIME=%08lX%08lX OFFSET=%08lX%08lX NEXT0=%08lX%08lX NEXT1=%08lX%08lX\r\n",
               (unsigned long)stHi, (unsigned long)stLo,
               (unsigned long)ofHi, (unsigned long)ofLo,
               (unsigned long)s0Hi, (unsigned long)s0Lo,
               (unsigned long)s1Hi, (unsigned long)s1Lo);
    }

    /* SYNC0/SYNC1 边沿周期与抖动（tick -> ns：1 tick = 10.416 ns） */
    {
        UINT16 hist[8];
        UINT32 n0 = diag->Sync0EdgeCount;
        UINT32 n1 = diag->Sync1EdgeCount;
        UINT32 avg0 = HW_GetSync0AvgTicks();
        UINT32 avg1 = HW_GetSync1AvgTicks();
        UINT32 phaseSum = (UINT32)diag->Sync1PhaseSumTicks;
        UINT32 phaseAvgTicks = (diag->Sync1PhaseN != 0U) ? (phaseSum / diag->Sync1PhaseN) : 0U;

        HW_GetSync1Hist(hist);
        printf("[SYNC] S0=%lu min/avg/max_ns=%lu/%lu/%lu S1=%lu min/avg/max_ns=%lu/%lu/%lu inv=%lu phase_min/avg/max_ns=%lu/%lu/%lu\r\n",
               (unsigned long)n0,
               (unsigned long)((diag->Sync0PeriodMinTicks * 1000UL) / 96UL),
               (unsigned long)((avg0 * 1000UL) / 96UL),
               (unsigned long)((diag->Sync0PeriodMaxTicks * 1000UL) / 96UL),
               (unsigned long)n1,
               (unsigned long)((diag->Sync1PeriodMinTicks * 1000UL) / 96UL),
               (unsigned long)((avg1 * 1000UL) / 96UL),
               (unsigned long)((diag->Sync1PeriodMaxTicks * 1000UL) / 96UL),
               (unsigned long)diag->Sync1InvalidCount,
               (unsigned long)((diag->Sync1PhaseMinTicks * 1000UL) / 96UL),
               (unsigned long)((phaseAvgTicks * 1000UL) / 96UL),
               (unsigned long)((diag->Sync1PhaseMaxTicks * 1000UL) / 96UL));
        printf("[SYH ] h_ns=%u,%u,%u,%u,%u,%u,%u,%u (25/50/100/200/500/1k/2k/>2k)\r\n",
               (unsigned)hist[0], (unsigned)hist[1], (unsigned)hist[2], (unsigned)hist[3],
               (unsigned)hist[4], (unsigned)hist[5], (unsigned)hist[6], (unsigned)hist[7]);
    }

    /* TIM4 实际中断间隔：判断软件 1 ms tick 是否等于真实 1 ms（TIM3 0.5 us/tick） */
    {
        UINT32 n4 = diag->Tim4DtN;
        UINT32 avg4 = (n4 != 0U) ? (diag->Tim4DtSumTicks / n4) : 0U;

        printf("[TICK] TIM4 n=%lu min/avg/max_us=%lu/%lu/%lu tick=%lu\r\n",
               (unsigned long)n4,
               (unsigned long)(diag->Tim4DtMinTicks / 2UL),
               (unsigned long)(avg4 / 2UL),
               (unsigned long)(diag->Tim4DtMaxTicks / 2UL),
               (unsigned long)BSP_Get1msTick());
    }

    /* SYNC1(PB11) 电平采样已移到 PrintDcCompact（每 1 s 入口、内部 5 s 一采） */

    PrintPdBytes("SM2O", 0x1200U, snap.pdOut);
    printf("[SM2V] samples=%lu A(5A0034127856)=%lu B(A50034127856)=%lu other=%lu lastOther=%02X %02X %02X %02X %02X %02X\r\n",
           (unsigned long)s_pdOutSamples,
           (unsigned long)s_pdOutAMatch,
           (unsigned long)s_pdOutBMatch,
           (unsigned long)s_pdOutOtherCount,
           (unsigned)s_pdOutOtherBytes[0], (unsigned)s_pdOutOtherBytes[1],
           (unsigned)s_pdOutOtherBytes[2], (unsigned)s_pdOutOtherBytes[3],
           (unsigned)s_pdOutOtherBytes[4], (unsigned)s_pdOutOtherBytes[5]);
    s_pdOutSamples = 0U;
    s_pdOutAMatch = 0U;
    s_pdOutBMatch = 0U;
    s_pdOutOtherCount = 0U;
    /* SM3 为“ECAT 读 / PDI 写”的 3 缓冲区模式，PDI 回读 0x1300 地址区恒为 0
       （写入后立即回读也为 0），不能作为 TxPDO 观测手段。这里打印应用实际写入 TxPDO
       的影子副本（el9800appl.c 记录），主站侧 slave.input 应与其一致。 */
    {
        UINT8 k;

        printf("[TXPD] map=");
        for (k = 0U; k < 6U; k++)
        {
            printf("%02X", (unsigned)g_DiagPdInMap[k]);
        }
        printf("\r\n");
    }

    printf("[SM  ] SM2 A=%04X L=%04X C=%02X S=%02X EN=%02X | SM3 A=%04X L=%04X C=%02X S=%02X EN=%02X\r\n",
           (unsigned)snap.sm[2][0], (unsigned)snap.sm[2][1],
           (unsigned)(snap.sm[2][2] & 0xFFU), (unsigned)(snap.sm[2][2] >> 8),
           (unsigned)(snap.sm[2][3] & 0xFFU),
           (unsigned)snap.sm[3][0], (unsigned)snap.sm[3][1],
           (unsigned)(snap.sm[3][2] & 0xFFU), (unsigned)(snap.sm[3][2] >> 8),
           (unsigned)(snap.sm[3][3] & 0xFFU));

    if (diag->FirstOutTick != 0U)
    {
        sinceOut = (int32_t)(now - diag->FirstOutTick);
        printf("[FIRST] firstOutTick=%lu (+%ld ms) firstEventTick=%lu\r\n",
               (unsigned long)diag->FirstOutTick, (long)sinceOut,
               (unsigned long)diag->FirstEventTick);
    }
}

int main(void)
{
    uint32_t hwResult;
    uint32_t lastPrintTick;
    uint32_t lastIrqmTick;
    uint32_t lastDcsTick;
    uint8_t lastAlStatus;
    uint8_t lastEscInt;
    const BSP_E252Info *e252;

    /* 与官方 GPIO_Toggle 例程一致：先按 RCC 实际状态刷新时钟，再初始化延时 */
    SystemCoreClockUpdate();
    Delay_Init();

    hwResult = HW_Init();
    e252 = BSP_GetE252Info();

    if (hwResult != 0U)
    {
        printf("\r\n[E252] HW_Init FAILED, code=%lu selftest=%d BYTE_TEST=0x%08lX ID_REV=0x%08lX\r\n",
               (unsigned long)hwResult, (int)e252->result,
               (unsigned long)e252->byteTest, (unsigned long)e252->idRev);

        /* E252 自检或 PDI 配置失败：错误灯慢闪，停在安全循环 */
        for (;;)
        {
            BSP_SetErrorLed(1);
            Delay_Ms(500);
            BSP_SetErrorLed(0);
            Delay_Ms(500);
        }
    }

    printf("\r\n[E252] CH32V307 ECAT boot, SYSCLK=%lu Hz\r\n",
           (unsigned long)SystemCoreClock);
    printf("[E252] HW_Init=0 selftest=0 BYTE_TEST=0x%08lX ID_REV=0x%08lX\r\n",
           (unsigned long)e252->byteTest, (unsigned long)e252->idRev);

    /* 时钟启动证据：路径/重试/寄存器/串口实际分频（BRR 为 16 分频计数，
     * baud = PCLK2 / BRR，见 RM 18.3 与 18.10.3） */
    {
        RCC_ClocksTypeDef clocks;
        uint32_t ctlr = RCC->CTLR;
        uint32_t cfgr = RCC->CFGR0;
        uint32_t exten = EXTEN->EXTEN_CTR;
        uint32_t brr = (uint32_t)(USART1->BRR & 0xFFFFU);
        uint32_t baud;
        const char *pathName = (g_ClkDiagPath == 0U) ? "HSE-PLL"
                             : (g_ClkDiagPath == 1U) ? "HSI-PLL"
                             : "HSI-DIRECT";

        RCC_GetClocksFreq(&clocks);
        baud = (brr != 0U) ? (clocks.PCLK2_Frequency / brr) : 0U;

        printf("[CLK ] path=%s attempts=%lu ok_attempt=%lu ok_loops=%lu valid=0x%02lX pll_loops=%lu fallback=%lu loops=[%lu,%lu,%lu,%lu,%lu]\r\n",
               pathName,
               (unsigned long)g_ClkDiagAttempts,
               (unsigned long)g_ClkDiagOkAttempt,
               (unsigned long)g_ClkDiagOkLoops,
               (unsigned long)g_ClkDiagValid,
               (unsigned long)g_ClkDiagPllLoops,
               (unsigned long)g_ClkDiagFallback,
               (unsigned long)g_HseLoops[0], (unsigned long)g_HseLoops[1],
               (unsigned long)g_HseLoops[2], (unsigned long)g_HseLoops[3],
               (unsigned long)g_HseLoops[4]);

        printf("[CLK ] CTLR=0x%08lX CFGR0=0x%08lX EXTEN=0x%08lX HSEON=%lu HSERDY=%lu PLLRDY=%lu PLLSRC=%lu SWS=%lu HSIPRE=%lu\r\n",
               (unsigned long)ctlr, (unsigned long)cfgr, (unsigned long)exten,
               (unsigned long)((ctlr & RCC_HSEON) ? 1U : 0U),
               (unsigned long)((ctlr & RCC_HSERDY) ? 1U : 0U),
               (unsigned long)((ctlr & RCC_PLLRDY) ? 1U : 0U),
               (unsigned long)((cfgr & RCC_PLLSRC) ? 1U : 0U),
               (unsigned long)((cfgr & RCC_SWS) >> 2),
               (unsigned long)((exten & EXTEN_PLL_HSI_PRE) ? 1U : 0U));

        printf("[CLK ] SYSCLK=%lu HCLK=%lu PCLK1=%lu PCLK2=%lu BRR=0x%04lX baud_calc=%lu target=115200\r\n",
               (unsigned long)clocks.SYSCLK_Frequency,
               (unsigned long)clocks.HCLK_Frequency,
               (unsigned long)clocks.PCLK1_Frequency,
               (unsigned long)clocks.PCLK2_Frequency,
               (unsigned long)brr, (unsigned long)baud);

        /* 计时标定：TIM2 计 500 µs（Delay_Us 基于 SysTick），并打印 SPI2 CR1.BR */
        {
            UINT32 t0 = TIM2->CNT;
            UINT32 t1;
            UINT32 i;
            UINT32 sumAl = 0U;
            UINT32 sumRaw = 0U;
            UINT32 maxAl = 0U;
            UINT16 alVal = 0U;
            Delay_Us(500U);
            t1 = TIM2->CNT;
            printf("[TIME] tim2_500us_ticks=%lu spi_cr1=0x%04lX\r\n",
                   (unsigned long)(uint16_t)(t1 - t0),
                   (unsigned long)(E252_SPI->CTLR1 & 0xFFFFU));

            /* 单次 CSR 读 AL Event 与单次裸 SPI 4 字节读的耗时对比（ESC 空闲时） */
            for (i = 0U; i < 20U; i++)
            {
                UINT32 d;
                t0 = TIM2->CNT;
                HW_EscRead((MEM_ADDR *)&alVal, 0x0220U, 2U);
                t1 = TIM2->CNT;
                d = (uint32_t)(uint16_t)(t1 - t0);
                sumAl += d;
                if (d > maxAl)
                {
                    maxAl = d;
                }

                t0 = TIM2->CNT;
                (void)PDI_PortRead32(0x0304U);
                t1 = TIM2->CNT;
                sumRaw += (uint32_t)(uint16_t)(t1 - t0);
            }
            printf("[PBEN] al_csr_avg/max_us=%lu/%lu raw4b_avg_us=%lu al=0x%04X\r\n",
                   (unsigned long)((sumAl / 20U) / BOARD_TICKS_PER_US),
                   (unsigned long)(maxAl / BOARD_TICKS_PER_US),
                   (unsigned long)((sumRaw / 20U) / BOARD_TICKS_PER_US),
                   (unsigned)alVal);

            /* 纯 SPI 字节吞吐标定：单个 CS 事务连续收发 8 / 64 字节，不含 ESC 命令 */
            {
                UINT32 r8  = E252_SPI_RawBench(8U);
                UINT32 r64 = E252_SPI_RawBench(64U);
                UINT32 alChk = 0U;

                HW_EscRead((MEM_ADDR *)&alChk, 0x0220U, 2U);
                printf("[PBEN2] raw8_us=%lu raw64_us=%lu byte_ns=%lu dmaerr=%lu al_after=0x%04X\r\n",
                       (unsigned long)(r8 / BOARD_TICKS_PER_US),
                       (unsigned long)(r64 / BOARD_TICKS_PER_US),
                       (unsigned long)((r64 * 1000UL) / (64UL * BOARD_TICKS_PER_US)),
                       (unsigned long)E252_SPI_GetDmaTimeouts(),
                       (unsigned)alChk);
            }

            /* 检查 ESC 核心寄存器能否直接线性寻址（若可以，AL Event 读
             * 可从 3 次 CSR 事务降为 1 次 FASTREAD）。用 AL Status(0x0130)
             * 这类已知非零值做对照，避免 0x0220 恒 0 时“两边一样”的假阳性。 */
            {
                UINT32 alStDir = PDI_PortRead32(0x0130U);
                UINT32 alStCsr = 0U;

                HW_EscRead((MEM_ADDR *)&alStCsr, 0x0130U, 2U);
                printf("[DIR ] al_status: direct=0x%08lX csr=0x%08lX\r\n",
                       (unsigned long)alStDir,
                       (unsigned long)alStCsr);
            }
        }

        if (g_ClkDiagFallback != 0U)
        {
            printf("!!!! [CLK] HSE STARTUP FAILED (%lu tries) -> %s fallback, SYSCLK=%lu Hz !!!!\r\n",
                   (unsigned long)g_ClkDiagAttempts, pathName,
                   (unsigned long)clocks.SYSCLK_Frequency);
            printf("!!!! [CLK] clock is NOT crystal-grade -> EtherCAT OP transition will be refused !!!!\r\n");
        }
    }

    /* 无主站可观测项：ESC EEPROM 控制/状态 0x0502（bit11=CRC 错误，bit12=未装载，bit15=忙） */
    {
        UINT16 eepromStatus = 0;
        uint32_t retry;

        HW_EscReadWord(eepromStatus, 0x0502);
        for (retry = 0U; (retry < 20U) && ((eepromStatus & 0x8000U) != 0U); retry++)
        {
            /* 上电自动装载 EEPROM 期间 bit15=忙，等待其完成后再判定 */
            Delay_Ms(10);
            HW_EscReadWord(eepromStatus, 0x0502);
        }
        printf("[E252] ESC 0x0502=0x%04X CRC=%u LOAD=%u BUSY=%u A2B=%u (1=error)\r\n",
               (unsigned)eepromStatus,
               (unsigned)((eepromStatus & 0x0800U) ? 1U : 0U),
               (unsigned)((eepromStatus & 0x1000U) ? 1U : 0U),
               (unsigned)((eepromStatus & 0x8000U) ? 1U : 0U),
               (unsigned)((eepromStatus & 0x0080U) ? 1U : 0U));
    }

    /* IRQ 极性与空闲电平自检（只在 INIT/PREOP 做，避免干扰主站过程数据） */
    {
        UINT16 alNow = 0U;
        UINT8 lvNormal = 1U;
        UINT8 lvInverted = 1U;
        UINT32 cfgNormal = 0U;

        HW_EscReadWord(alNow, 0x0130U);
        if ((alNow & 0x000FU) < 0x0004U)
        {
            HW_IrqPolarityProbe(&lvNormal, &lvInverted, &cfgNormal);
            printf("[IRQPOL] AL=0x%02X IRQCFG=0x%08lX idle_normal=%u idle_inverted=%u (expect 1/0)\r\n",
                   (unsigned)(alNow & 0xFFU), (unsigned long)cfgNormal,
                   (unsigned)lvNormal, (unsigned)lvInverted);
        }
    }

#if E252_PDI_CSR_PROBE
    /* 一次性 CSR 探针：地址 × 宽度矩阵，打印每条命令的轮询次数/耗时/返回值。
     * 若某条命令让 CSR busy 永不清零，后续 EtherCAT 无法运行，停在错误灯循环。 */
    if (PDI_CsrProbeRun() == 0U)
    {
        printf("[CSRP] probe aborted: ESC CSR busy is stuck, halt for capture\r\n");
        for (;;)
        {
            BSP_SetErrorLed(1);
            Delay_Ms(200);
            BSP_SetErrorLed(0);
            Delay_Ms(200);
        }
    }
#endif

    (void)MainInit();

    /* FreeRun 模式必须屏蔽 EXTI3：ESC 的 IRQ 在 AL Event 未被读取期间保持低电平，
     * 此时 CH32 EXTI 的挂起位会被持续置位（软件无法清除），形成中断风暴把主循环饿死。
     * FreeRun 本来就由主循环轮询 0x0220，不需要 IRQ 中断 */
    NVIC_DisableIRQ(EXTI3_IRQn);
    EXTI_ClearITPendingBit(EXTI_Line3);

    lastPrintTick = BSP_Get1msTick();
    lastIrqmTick = lastPrintTick;
    lastDcsTick = lastPrintTick;
    lastAlStatus = nAlStatus;
    lastEscInt = (uint8_t)bEscIntEnabled;
    for (;;)
    {
        uint32_t now;

        MainLoop();

        now = BSP_Get1msTick();
        SamplePdOutPattern(now);

        /* 进 OP 后把上电残留的“无挂起位”计数归零，使统计窗口内 nf 从 0 起算 */
        if ((nAlStatus == 0x08U) && (s_opNfReset == 0U))
        {
            HW_DIAG *d = HW_GetDiag();
            s_opNfReset = 1U;
            printf("[DIAG] OP entered, Exti3NoFlag reset (boot_nf=%lu)\r\n",
                   (unsigned long)d->Exti3NoFlag);
            d->Exti3NoFlag = 0U;
        }

        /* 在连续帧流中反相 IRQ 极性，检查“低有效 + 下降沿”这条路径 */
        if ((nAlStatus == 0x08U) && (s_irqEdgeDone == 0U) &&
            (HW_GetDiag()->OutEventCount > 100U))
        {
            UINT32 edgesInv = 0U;
            UINT32 edgesNormal = 0U;
            UINT8 lvlInv = 1U;
            UINT8 lvlNormal = 1U;

            s_irqEdgeDone = 1U;
            HW_IrqEdgeProbe(&edgesInv, &edgesNormal, &lvlInv, &lvlNormal);
            printf("[IRQEDGE] lvl_inv=%u lvl_normal=%u d_edges_inv=%lu d_edges_normal=%lu\r\n",
                   (unsigned)lvlInv, (unsigned)lvlNormal,
                   (unsigned long)edgesInv, (unsigned long)edgesNormal);
        }

        /* 输出事件稳定后测一次 PA3 低电平（20 ms 窗口，暂停 EXTI3 且无人读 AL Event：
         * 电平保持型 IRQ 的低电平总时长≈整个窗口，短脉冲型只累计几十 µs） */
        if ((nAlStatus == 0x08U) && (s_irqPulseDone == 0U) &&
            (HW_GetDiag()->OutEventCount > 300U))
        {
            UINT32 pulses = 0U;
            UINT32 minNs = 0U;
            UINT32 avgNs = 0U;
            UINT32 maxNs = 0U;
            UINT32 lowUs = 0U;
            UINT8 endLow = 1U;

            s_irqPulseDone = 1U;
            BSP_MeasureIrqPulse(20U, &pulses, &minNs, &avgNs, &maxNs, &lowUs, &endLow);
            printf("[IRQP] win=20ms pulses=%lu low_ns=%lu/%lu/%lu low_us_total=%lu endLow=%u\r\n",
                   (unsigned long)pulses, (unsigned long)minNs,
                   (unsigned long)avgNs, (unsigned long)maxNs,
                   (unsigned long)lowUs, (unsigned)endLow);
        }

        /* FreeRun <-> SM 模式切换：FreeRun 期间 EXTI3 处于屏蔽状态（见上），
           切到 SM 模式时先清挂起位；若 IRQ 已为低电平、下降沿不会再来，
           补一次软件触发，保证首个过程数据事件不会丢（否则 SAFEOP→OP 超时 0x001B） */
        if (bEscIntEnabled != FALSE)
        {
            if (lastEscInt == 0U)
            {
                EXTI_ClearITPendingBit(EXTI_Line3);
                if (BSP_GetIrqLevel() == 0U)
                {
                    EXTI_GenerateSWInterrupt(EXTI_Line3);
                }
                NVIC_EnableIRQ(EXTI3_IRQn);
            }
        }
        else if (lastEscInt != 0U)
        {
            NVIC_DisableIRQ(EXTI3_IRQn);
            EXTI_ClearITPendingBit(EXTI_Line3);
        }
        lastEscInt = (uint8_t)bEscIntEnabled;

        /* 状态跳变立即打印（含状态码），便于与主站/Wireshark 时间轴对齐 */
        if (nAlStatus != lastAlStatus)
        {
            UINT16 code = 0U;
            UINT16 ctl = 0U;

            lastAlStatus = nAlStatus;
            HW_EscReadWord(code, 0x0134U);
            HW_EscReadWord(ctl, 0x0120U);
            printf("[AL  ] t=%lu nAlStatus=0x%02X CODE=0x%04X ALControl=0x%02X FRAME=%u\r\n",
                   (unsigned long)now, (unsigned)nAlStatus,
                   (unsigned)code, (unsigned)(ctl & 0xFFU),
                   (unsigned)bEcatFirstOutputsReceived);
        }

        /* 轻量计数行：200 ms 一行，便于外部工具按相位对齐 EXT3/PDI 增量 */
        if ((uint32_t)(now - lastIrqmTick) >= 200U)
        {
            HW_DIAG *d = HW_GetDiag();
            lastIrqmTick = now;
            printf("[IRQM] t=%lu E=%lu P=%lu nf=%lu\r\n",
                   (unsigned long)now, (unsigned long)d->Exti3Count,
                   (unsigned long)d->PdiIsrCount, (unsigned long)d->Exti3NoFlag);
        }

        /* 紧凑 DC/SYNC 行：1 s 一行，降低串口负载（尾部长跑抓取不再丢块） */
        if ((uint32_t)(now - lastDcsTick) >= 1000U)
        {
            lastDcsTick = now;
            PrintDcCompact(now);
        }

        if ((uint32_t)(now - lastPrintTick) >= 5000U)
        {
            lastPrintTick = now;
            PrintDiag(now);
        }
    }
}
