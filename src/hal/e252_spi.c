/**
 * @file    e252_spi.c
 * @brief   E252 SPI PDI 平台钩子实现（SPI2 / 软件片选 / 事务间隔）
 *
 * 时序依据：SYS-E252 数据手册 §6.2（背靠背访问间隔）与 §11（SPI 指令/地址格式）
 *   - READ 0x03  <= 30 MHz
 *   - FASTREAD 0x0B <= 80 MHz，首字节为空周期
 *   - WRITE 0x02
 *   - 两字节地址，MSB 先；地址 bit15:14 指定递增/递减/固定
 *   - 数据 32-bit 小端；SCS# 最小无效时间 50 ns
 */

#include "hal/e252_spi.h"
#include "bsp/board_config.h"

/* 事务间隔：SYS-E252 手册要求 SCS# 高电平 ≥50 ns（96 MHz 下约 5 个周期）。
 * nop 延时用单条 asm 展开 16 个 nop（≈167 ns）；若写成 for 循环且未展开，
 * 24 次迭代约 2.6 µs，占单次 CSR 事务 7 µs 的三分之一。展开后满足 50 ns
 * 下限并留 3 倍余量，且不随编译优化变化。 */
static void E252_SPI_Gap(void)
{
    __asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;"
                   "nop; nop; nop; nop; nop; nop; nop; nop");
}

/* 临界区只清/置 mstatus.MIE（bit3），禁止使用 __disable_irq()。
 * 原因：WCH 的 __disable_irq() 是 csrc mstatus,0x88，会同时清掉 MPIE(bit7)。
 * 在 ISR 内执行时 mstatus 为 0x6080（MIE 已被硬件清零、MPIE=1），
 * 一旦被这条指令改成 0x6000，mret 就按 MIE=MPIE=0 返回，主循环从此永久关中断。
 * 故障表现：s_tick1ms 停在 1，TIM4(IRQ46)/EXTI 请求在 PFIC ISR 中一直挂起。
 * 这里用 csrc/csrs 0x300,0x8 只动 MIE，MPIE 保持不变，mret 语义正确。 */
static uint32_t E252_EnterCritical(void)
{
    uint32_t mstatus;
    __asm volatile("csrr %0, 0x300" : "=r"(mstatus));
    __asm volatile("csrc 0x300, %0" :: "r"(0x8U) : "memory");
    return mstatus & 0x8U; /* 仅返回 MIE */
}

static void E252_ExitCritical(uint32_t mieWasSet)
{
    if (mieWasSet != 0U)
    {
        __asm volatile("csrs 0x300, %0" :: "r"(0x8U) : "memory");
    }
}

static UINT32 s_lockStartTick;
static UINT32 s_lockMaxTicks;

UINT32 E252_GetPdiLockMaxTicks(void)
{
    return s_lockMaxTicks;
}

UINT32 E252_PdiLock(void)
{
    s_lockStartTick = TIM2->CNT;
    return E252_EnterCritical();
}

void E252_PdiUnlock(UINT32 state)
{
    UINT32 dt = (uint16_t)(TIM2->CNT - s_lockStartTick);

    if (dt > s_lockMaxTicks)
    {
        s_lockMaxTicks = dt;
    }
    E252_ExitCritical(state);
}

#if !E252_SPI_USE_DMA
static uint8_t E252_SPI_TxRx(uint8_t txData)
{
    /* 直接寄存器访问：SPI_I2S_GetFlagStatus() 是外部函数，每字节调用两次，
     * 在 23 字节的 CSR 读里被放大到数十 µs（最小约 45 µs）。
     * STATR.TXE=bit1、RXNE=bit0，DATAR 读写与库函数等价。 */
    while ((E252_SPI->STATR & 0x0002U) == 0U)
    {
    }
    E252_SPI->DATAR = (uint16_t)txData;

    while ((E252_SPI->STATR & 0x0001U) == 0U)
    {
    }
    return (uint8_t)E252_SPI->DATAR;
}
#endif

/* -------------------------------------------------------------------------- */
/* 一次 CS 有效期内的整段收发（DMA 或逐字节轮询，编译期可选）                */
/* DMA 通道映射（RM Figure 11-1，与 STM32F1 兼容）：                            */
/*   SPI2_RX -> DMA1_Channel4，SPI2_TX -> DMA1_Channel5                         */
/*   SPI1_RX -> DMA1_Channel2，SPI1_TX -> DMA1_Channel3                         */
/* -------------------------------------------------------------------------- */
#if E252_SPI_USE_SPI1
#define E252_DMA_RX_CH          DMA1_Channel2
#define E252_DMA_TX_CH          DMA1_Channel3
#define E252_DMA_TC_RX          (1UL << 5)      /* DMA1 INTFR: TCIF2 */
#define E252_DMA_TC_TX          (1UL << 9)      /* DMA1 INTFR: TCIF3 */
#define E252_DMA_FLAG_CLR       (0x00000FF0UL)  /* 通道 2/3 的 GIF/TCIF/HTIF/TEIF */
#else
#define E252_DMA_RX_CH          DMA1_Channel4
#define E252_DMA_TX_CH          DMA1_Channel5
#define E252_DMA_TC_RX          (1UL << 13)     /* DMA1 INTFR: TCIF4 */
#define E252_DMA_TC_TX          (1UL << 17)     /* DMA1 INTFR: TCIF5 */
#define E252_DMA_FLAG_CLR       (0x000FF000UL)  /* 通道 4/5 的 GIF/TCIF/HTIF/TEIF */
#endif
#define E252_DMA_CFGR_RX        (0x00002080UL)  /* MINC, DIR=外设->内存, PL=高 */
#define E252_DMA_CFGR_TX        (0x00002090UL)  /* MINC, DIR=内存->外设, PL=高 */
#define E252_SPI_CTLR2_DMAEN    0x0003U         /* RXDMAEN | TXDMAEN */
#define E252_DMA_WAIT_GUARD     20000UL

static UINT32 s_dmaTimeoutCount;

UINT32 E252_SPI_GetDmaTimeouts(void)
{
    return s_dmaTimeoutCount;
}

#if E252_SPI_USE_DMA
static void E252_SPI_Burst(const uint8_t *tx, uint8_t *rx, UINT16 n)
{
    UINT32 guard;

    /* 先关通道再清标志：避免上一事务的残留状态参与当前传输 */
    E252_DMA_TX_CH->CFGR = 0U;
    E252_DMA_RX_CH->CFGR = 0U;
    DMA1->INTFCR = E252_DMA_FLAG_CLR;    /* 清本实例两个通道的 GIF/TCIF/HTIF/TEIF */

    /* 清 SPI 接收残留（RXNE/OVR）：否则首字节错位一个周期 */
    while ((E252_SPI->STATR & 0x0001U) != 0U)
    {
        (void)E252_SPI->DATAR;
    }
    (void)E252_SPI->STATR;
    (void)E252_SPI->DATAR;

    E252_DMA_RX_CH->PADDR = (uint32_t)&E252_SPI->DATAR;
    E252_DMA_RX_CH->MADDR = (uint32_t)rx;
    E252_DMA_RX_CH->CNTR  = (uint32_t)n;
    E252_DMA_TX_CH->PADDR = (uint32_t)&E252_SPI->DATAR;
    E252_DMA_TX_CH->MADDR = (uint32_t)tx;
    E252_DMA_TX_CH->CNTR  = (uint32_t)n;

    __asm volatile("" ::: "memory");

    /* 先使能 RX 通道再使能 TX，最后打开 SPI 的 DMA 请求位 */
    E252_DMA_RX_CH->CFGR = E252_DMA_CFGR_RX | 0x1UL;
    E252_DMA_TX_CH->CFGR = E252_DMA_CFGR_TX | 0x1UL;
    E252_SPI->CTLR2 |= E252_SPI_CTLR2_DMAEN;

    /* RX 传送完成即代表最后一个字节已经移入，天然晚于 TX 完成 */
    guard = 0U;
    while ((DMA1->INTFR & E252_DMA_TC_RX) == 0U)
    {
        if (++guard > E252_DMA_WAIT_GUARD)
        {
            s_dmaTimeoutCount++;
            break;
        }
    }
    guard = 0U;
    while ((DMA1->INTFR & E252_DMA_TC_TX) == 0U)
    {
        if (++guard > E252_DMA_WAIT_GUARD)
        {
            s_dmaTimeoutCount++;
            break;
        }
    }
    /* 等 SPI 移位寄存器彻底空闲，再允许拉高 SCS# */
    guard = 0U;
    while ((E252_SPI->STATR & 0x0080U) != 0U)
    {
        if (++guard > E252_DMA_WAIT_GUARD)
        {
            s_dmaTimeoutCount++;
            break;
        }
    }

    E252_SPI->CTLR2 &= (uint16_t)~E252_SPI_CTLR2_DMAEN;
    E252_DMA_TX_CH->CFGR = 0U;
    E252_DMA_RX_CH->CFGR = 0U;
    DMA1->INTFCR = E252_DMA_FLAG_CLR;
}
#else
static void E252_SPI_Burst(const uint8_t *tx, uint8_t *rx, UINT16 n)
{
    UINT16 i;

    for (i = 0U; i < n; i++)
    {
        rx[i] = E252_SPI_TxRx(tx[i]);
    }
}
#endif

/* CS 拉低到首个 SCK 之间人为插入的等待。
 * 正式版本 E252_CS_ASSERT_GAP_US=0，此函数为空。
 * 96 MHz 下 1 µs ≈ 96 个空转 tick（按 1 循环≈1 tick 估算，仅用于定性）。 */
static void E252_CS_AssertGap(void)
{
#if (E252_CS_ASSERT_GAP_US > 0U)
    UINT32 i;
    for (i = 0U; i < (BOARD_TICKS_PER_US * (UINT32)E252_CS_ASSERT_GAP_US); i++)
    {
        __asm volatile("nop");
    }
#endif
}

static void E252_CS_Assert(void)
{
    GPIO_ResetBits(E252_SPI_CS_PORT, E252_SPI_CS_PIN);
    E252_CS_AssertGap();
}

static void E252_CS_Deassert(void)
{
    GPIO_SetBits(E252_SPI_CS_PORT, E252_SPI_CS_PIN);
    E252_SPI_Gap();
}

void E252_SPI_Init(void)
{
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef spi;

    GPIO_SetBits(E252_SPI_CS_PORT, E252_SPI_CS_PIN);

    /* SCK / MOSI 复用推挽（SPI1 默认映射：PA5 / PA7） */
    gpio.GPIO_Pin = E252_SPI_SCK_PIN | E252_SPI_MOSI_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(E252_SPI_PIN_PORT, &gpio);

    /* MISO 上拉输入（PA6） */
    gpio.GPIO_Pin = E252_SPI_MISO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(E252_SPI_PIN_PORT, &gpio);

    SPI_I2S_DeInit(E252_SPI);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
#if (E252_SPI_TEST_MODE == 1)
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_2Edge;
#elif (E252_SPI_TEST_MODE == 2)
    spi.SPI_CPOL = SPI_CPOL_High;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
#elif (E252_SPI_TEST_MODE == 3)
    spi.SPI_CPOL = SPI_CPOL_High;
    spi.SPI_CPHA = SPI_CPHA_2Edge;
#else
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
#endif
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = E252_SPI_PRESCALER;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial = 7;
    SPI_Init(E252_SPI, &spi);

#if E252_SPI_HSRXEN
    /* 高速读模式：必须在 SPE 置位前写入（RM §20.4.10）。置位后 BR[2:0]
     * 映射为 /2../9，例如 BR=001 → 32 MHz、BR=000 → 48 MHz。 */
    E252_SPI->HSCR |= SPI_HSCR_HSRXEN;
#endif

    SPI_Cmd(E252_SPI, ENABLE);

    /* 丢弃使能后 RX 缓冲中的残留字节：否则首次事务整体错位一个字节，
     * 首个 BYTE_TEST 会读到 0xFFFFFFFF，而紧随其后的 ID_REV 正常。 */
    while ((E252_SPI->STATR & 0x0001U) != 0U)
    {
        (void)E252_SPI->DATAR;
    }

#if E252_SPI_USE_DMA
    /* DMA1 挂在 AHB，必须先开时钟；通道在每次事务开始时才配置 */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
#endif
}

/* 一次 CS 事务的收发缓冲：命令段与数据段在同一突发里完成，
 * 读事务的发送字节固定 0xFF（FASTREAD 空周期/哑元）。
 * 使用静态缓冲而非栈：PDI 事务由临界区串行化，不存在重入。 */
#define E252_TXN_BUF_MAX    64U

static uint8_t s_txnTx[8];
static uint8_t s_txnRx[8];

UINT32 PDI_PortRead32(UINT16 address)
{
    uint32_t mieWasSet;
    uint8_t *tx = s_txnTx;
    uint8_t *rx = s_txnRx;
    uint16_t addrField = (uint16_t)((address & E252_ADDR_MASK) | E252_ADDR_MODE_INC);

    tx[0] = E252_CMD_FAST_READ;
    tx[1] = (uint8_t)((addrField >> 8) & 0xFFU);
    tx[2] = (uint8_t)(addrField & 0xFFU);
    tx[3] = 0xFFU;  /* FASTREAD 空周期 */
    tx[4] = 0xFFU;
    tx[5] = 0xFFU;
    tx[6] = 0xFFU;
    tx[7] = 0xFFU;

    mieWasSet = E252_EnterCritical();

    E252_CS_Assert();
    E252_SPI_Burst(tx, rx, 8U);
    E252_CS_Deassert();

    E252_ExitCritical(mieWasSet);

    return ((uint32_t)rx[4]) |
           (((uint32_t)rx[5]) << 8) |
           (((uint32_t)rx[6]) << 16) |
           (((uint32_t)rx[7]) << 24);
}

UINT32 E252_SPI_RawBench(UINT32 nBytes)
{
    static uint8_t tx[E252_TXN_BUF_MAX];
    static uint8_t rx[E252_TXN_BUF_MAX];
    UINT32 mieWasSet;
    UINT32 t0;
    UINT32 t1;
    UINT32 i;

    if (nBytes > E252_TXN_BUF_MAX)
    {
        nBytes = E252_TXN_BUF_MAX;
    }
    for (i = 0U; i < nBytes; i++)
    {
        tx[i] = 0xFFU;
    }

    mieWasSet = E252_EnterCritical();
    E252_CS_Assert();
    t0 = TIM2->CNT;
    E252_SPI_Burst(tx, rx, (UINT16)nBytes);
    t1 = TIM2->CNT;
    E252_CS_Deassert();
    E252_ExitCritical(mieWasSet);

    return (UINT32)(UINT16)(t1 - t0);
}

UINT8 PDI_PortRead8(UINT16 address)
{
    uint32_t mieWasSet;
    uint8_t *tx = s_txnTx;
    uint8_t *rx = s_txnRx;
    uint16_t addrField = (uint16_t)((address & E252_ADDR_MASK) | E252_ADDR_MODE_INC);

    tx[0] = E252_CMD_FAST_READ;
    tx[1] = (uint8_t)((addrField >> 8) & 0xFFU);
    tx[2] = (uint8_t)(addrField & 0xFFU);
    tx[3] = 0xFFU;  /* FASTREAD 空周期 */
    tx[4] = 0xFFU;

    mieWasSet = E252_EnterCritical();

    E252_CS_Assert();
    E252_SPI_Burst(tx, rx, 5U);
    E252_CS_Deassert();

    E252_ExitCritical(mieWasSet);
    return rx[4];
}

void PDI_PortWrite32(UINT16 address, UINT32 value)
{
    uint32_t mieWasSet;
    uint8_t *tx = s_txnTx;
    uint8_t *rx = s_txnRx;
    uint16_t addrField = (uint16_t)((address & E252_ADDR_MASK) | E252_ADDR_MODE_INC);

    tx[0] = E252_CMD_WRITE;
    tx[1] = (uint8_t)((addrField >> 8) & 0xFFU);
    tx[2] = (uint8_t)(addrField & 0xFFU);
    tx[3] = (uint8_t)(value & 0xFFU);
    tx[4] = (uint8_t)((value >> 8) & 0xFFU);
    tx[5] = (uint8_t)((value >> 16) & 0xFFU);
    tx[6] = (uint8_t)((value >> 24) & 0xFFU);

    mieWasSet = E252_EnterCritical();

    E252_CS_Assert();
    E252_SPI_Burst(tx, rx, 7U);
    E252_CS_Deassert();

    E252_ExitCritical(mieWasSet);
}
