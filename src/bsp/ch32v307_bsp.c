/**
 * @file    ch32v307_bsp.c
 * @brief   CH32V307 + E252 DPORT 板级支持包实现
 */

#include "bsp/board_config.h"
#include "bsp/ch32v307_bsp.h"
#include "hal/e252_spi.h"
#include "debug.h"

static BSP_E252Info s_e252Info;
static uint8_t s_ledShadow;
static uint8_t s_switchShadow;
static int16_t s_analogShadow;
static uint16_t s_pwmShadow;
static volatile uint32_t s_tick1ms;
static uint8_t s_keyRaw;        /* 1=按下（PA2 为低） */
static uint8_t s_keyCnt;        /* 稳定计数（ms） */
static uint8_t s_keyStable;     /* 去抖后的按键状态 */

static void BSP_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio;

    /* E252 Q_RESET：先保持无效（高），由上电时序统一拉低复位 */
    GPIO_SetBits(E252_RESET_GPIO_PORT, E252_RESET_PIN);

    gpio.GPIO_Pin = E252_RESET_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(E252_RESET_GPIO_PORT, &gpio);

    gpio.GPIO_Pin = E252_SPI_CS_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(E252_SPI_CS_PORT, &gpio);
    GPIO_SetBits(E252_SPI_CS_PORT, E252_SPI_CS_PIN); /* CS 空闲为高 */

    /* E252 IRQ / SYNC0 为输入，默认上拉（IRQ 低有效） */
    gpio.GPIO_Pin = E252_IRQ_PIN | E252_SYNC0_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(E252_IRQ_GPIO_PORT, &gpio);

    /* SYNC1 已改 PB11（原 PA5 让给 SPI1_SCK） */
    gpio.GPIO_Pin = E252_SYNC1_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(E252_SYNC1_GPIO_PORT, &gpio);

    /* USER 按键（PA2）：R24 10 kΩ 上拉到 VCC，S2 按下为 GND；内部上拉作为冗余 */
    gpio.GPIO_Pin = BOARD_USER_KEY_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BOARD_USER_KEY_PORT, &gpio);
}

static void BSP_EXTI_Init(void)
{
    EXTI_InitTypeDef exti;
    NVIC_InitTypeDef nvic;

    GPIO_EXTILineConfig(E252_IRQ_PORTSOURCE, E252_IRQ_PINSOURCE);
    GPIO_EXTILineConfig(E252_SYNC0_PORTSOURCE, E252_SYNC0_PINSOURCE);
    GPIO_EXTILineConfig(E252_SYNC1_PORTSOURCE, E252_SYNC1_PINSOURCE);

    exti.EXTI_Line = EXTI_Line3;
    exti.EXTI_Mode = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Falling;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_Init(&exti);

    exti.EXTI_Line = EXTI_Line4;
    EXTI_Init(&exti);

    exti.EXTI_Line = EXTI_Line11;
    EXTI_Init(&exti);

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

    /* EXTI3 只配置向量与优先级，不在此使能：上电时 ESC 的 IRQ 可能已为低电平，
     * 使能后 CH32 的 EXTI3 挂起位会被持续置位形成中断风暴（见 main.c 说明）。
     * 由 main.c 在 FreeRun -> SM 模式切换时再 NVIC_EnableIRQ */
    nvic.NVIC_IRQChannel = EXTI3_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = DISABLE;
    NVIC_Init(&nvic);

    /* SYNC0/SYNC1 在 DC 未激活时无脉冲，使能 NVIC 不会产生中断；
     * 激活后由 ESC 输出边沿触发，用于 DC 时间戳统计。 */
    nvic.NVIC_IRQChannel = EXTI4_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    nvic.NVIC_IRQChannel = EXTI15_10_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 1;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

static void BSP_TIM_Init(void)
{
    TIM_TimeBaseInitTypeDef tim;
    NVIC_InitTypeDef nvic;

    /* TIM3：2 MHz 自由计数，供 EtherCAT 定时器使用 */
    tim.TIM_Prescaler = ECAT_TIMER_PSC;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    tim.TIM_Period = 0xFFFF;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM3, &tim);
    TIM_CounterModeConfig(TIM3, TIM_CounterMode_Up);
    TIM_SetCounter(TIM3, 0);
    TIM_Cmd(TIM3, ENABLE);

    /* TIM2：96 MHz 自由计数，IRQ 脉宽与中断延迟测量时基 */
    tim.TIM_Prescaler = 0;
    tim.TIM_Period = 0xFFFF;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM2, &tim);
    TIM_SetCounter(TIM2, 0);
    TIM_Cmd(TIM2, ENABLE);

    /* TIM4：1 ms 更新中断 */
    tim.TIM_Prescaler = ECAT_TIMER_1MS_PSC;
    tim.TIM_Period = ECAT_TIMER_1MS_ARR;
    TIM_TimeBaseInit(TIM4, &tim);
    TIM_SetCounter(TIM4, 0);
    TIM_ITConfig(TIM4, TIM_IT_Update, ENABLE);

    nvic.NVIC_IRQChannel = TIM4_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 3;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    TIM_Cmd(TIM4, ENABLE);
}

/* LED PWM：PA0=TIM2_CH1、PA1=TIM2_CH2。TIM2 已由 BSP_TIM_Init 配置为 96 MHz、
 * ARR=0xFFFF 自由计数（IRQ 延迟测量时基），此处只打开两路比较输出，不改 CNT/ARR，
 * 因此与 IRQ 延迟测量的时基互不影响。PWM 频率 = 96 MHz / 65536 ≈ 1.46 kHz。
 * LED 低有效：采用反相输出，CCR 直接表示“点亮时间”（0=灭，65535≈全亮）。 */
static void BSP_LedPwmInit(void)
{
    GPIO_InitTypeDef gpio;
    TIM_OCInitTypeDef oc;

    gpio.GPIO_Pin = BOARD_LED1_PIN | BOARD_LED2_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BOARD_LED1_PORT, &gpio);

    TIM_OCStructInit(&oc);
    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_OCPolarity = TIM_OCPolarity_Low;
    oc.TIM_Pulse = 0U;
    TIM_OC1Init(TIM2, &oc);
    TIM_OC2Init(TIM2, &oc);
    TIM_OC1PreloadConfig(TIM2, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM2, TIM_OCPreload_Enable);
}

static void BSP_E252_SelfTest(void)
{
    uint32_t byteTest;
    uint32_t idRev;

    /* Q_RESET 低脉冲复位 E252，随后释放并等待内部就绪 */
    GPIO_ResetBits(E252_RESET_GPIO_PORT, E252_RESET_PIN);
    Delay_Ms(2);
    GPIO_SetBits(E252_RESET_GPIO_PORT, E252_RESET_PIN);
    Delay_Ms(10);

    /* 首次事务丢弃，与 DSP 参考 PDI_AccessTest（连读两次 0x64）一致 */
    (void)PDI_PortRead32(0x0064U);
    byteTest = PDI_PortRead32(0x0064U); /* BYTE_TEST，期望 0x87654321 */
    idRev = PDI_PortRead32(0x0050U);    /* ID_REV */

    s_e252Info.byteTest = byteTest;
    s_e252Info.idRev = idRev;

    if (byteTest != 0x87654321UL)
    {
        s_e252Info.result = (byteTest == 0U || byteTest == 0xFFFFFFFFUL)
                                ? BSP_E252_SELFTEST_TIMEOUT
                                : BSP_E252_SELFTEST_BYTE_TEST_FAIL;
    }
    else if (idRev == 0U || idRev == 0xFFFFFFFFUL)
    {
        s_e252Info.result = BSP_E252_SELFTEST_ID_REV_FAIL;
    }
    else
    {
        s_e252Info.result = BSP_E252_SELFTEST_OK;
    }
}

void BSP_UartInit(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef uart;

    RCC_APB2PeriphClockCmd(BOARD_UART_GPIO_CLK | BOARD_UART_CLK, ENABLE);

    gpio.GPIO_Pin = BOARD_UART_TX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BOARD_UART_GPIO_PORT, &gpio);

    gpio.GPIO_Pin = BOARD_UART_RX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(BOARD_UART_GPIO_PORT, &gpio);

    uart.USART_BaudRate = BOARD_UART_BAUDRATE;
    uart.USART_WordLength = USART_WordLength_8b;
    uart.USART_StopBits = USART_StopBits_1;
    uart.USART_Parity = USART_Parity_No;
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(BOARD_UART, &uart);
    USART_Cmd(BOARD_UART, ENABLE);
}

void BSP_UartPutChar(char c)
{
    while (USART_GetFlagStatus(BOARD_UART, USART_FLAG_TXE) == RESET)
    {
    }
    USART_SendData(BOARD_UART, (uint16_t)(uint8_t)c);
}

void BSP_Init(void)
{
    Delay_Init();

#if E252_SPI_ON_APB2
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_AFIO | E252_SPI_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2 | RCC_APB1Periph_TIM3 |
                           RCC_APB1Periph_TIM4, ENABLE);
#else
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(E252_SPI_CLK | RCC_APB1Periph_TIM2 |
                           RCC_APB1Periph_TIM3 | RCC_APB1Periph_TIM4, ENABLE);
#endif

    BSP_UartInit();
    BSP_GPIO_Init();
    E252_SPI_Init();
    BSP_EXTI_Init();
    BSP_TIM_Init();
    BSP_LedPwmInit();

    BSP_E252_SelfTest();

    /* 清除可能在上电期间产生的 EXTI 挂起位 */
    EXTI_ClearITPendingBit(EXTI_Line3 | EXTI_Line4 | EXTI_Line11);
}

const BSP_E252Info *BSP_GetE252Info(void)
{
    return &s_e252Info;
}

/* mask bit0=LED1、bit1=LED2，1=亮（低有效 LED，内部换算成 CCR=65535 点亮） */
void BSP_ApplyLeds(uint8_t mask)
{
    s_ledShadow = mask;
    BSP_SetLedPwm(0U, ((mask & 0x01U) != 0U) ? 0xFFFFU : 0U);
    BSP_SetLedPwm(1U, ((mask & 0x02U) != 0U) ? 0xFFFFU : 0U);
}

void BSP_SetLedPwm(uint8_t ch, uint16_t duty)
{
    if (ch == 0U)
    {
        TIM_SetCompare1(TIM2, duty);
    }
    else
    {
        TIM_SetCompare2(TIM2, duty);
    }
}

void BSP_BreatheLeds(void)
{
    /* 0.5 Hz 三角波（2 s 周期）：LED1 渐亮时 LED2 渐暗，反之亦然 */
    uint32_t t = BSP_Get1msTick() % 2000U;
    uint16_t up;

    if (t < 1000U)
    {
        up = (uint16_t)(t * 65U);
    }
    else
    {
        up = (uint16_t)((2000U - t) * 65U);
    }
    BSP_SetLedPwm(0U, up);
    BSP_SetLedPwm(1U, (uint16_t)(0xFFFFU - up));
}

uint8_t BSP_GetLedShadow(void)
{
    return s_ledShadow;
}

uint8_t BSP_ReadSwitches(void)
{
    return s_switchShadow;
}

void BSP_KeyTick(void)
{
    /* PA2 低 = 按下。连续 20 ms 稳定后才更新，避免机械抖动 */
    uint8_t raw = (GPIO_ReadInputDataBit(BOARD_USER_KEY_PORT, BOARD_USER_KEY_PIN) == 0U) ? 1U : 0U;

    if (raw != s_keyRaw)
    {
        s_keyRaw = raw;
        s_keyCnt = 0U;
        return;
    }
    if (s_keyCnt < BOARD_USER_KEY_DEBOUNCE_MS)
    {
        s_keyCnt++;
    }
    if ((s_keyCnt >= BOARD_USER_KEY_DEBOUNCE_MS) && (s_keyStable != s_keyRaw))
    {
        s_keyStable = s_keyRaw;
        if (s_keyStable != 0U)
        {
            s_switchShadow |= 0x01U;
        }
        else
        {
            s_switchShadow = (uint8_t)(s_switchShadow & 0xFEU);
        }
    }
}

void BSP_ApplyAnalog(int16_t analogValue, uint16_t pwmValue)
{
    s_analogShadow = analogValue;
    s_pwmShadow = pwmValue;
}

uint32_t BSP_Get1msTick(void)
{
    return s_tick1ms;
}

void BSP_Inc1msTick(void)
{
    s_tick1ms++;
}

void BSP_SetErrorLed(uint8_t on)
{
    /* 板级无独立 ERR 灯：启动失败诊断时让 LED1/LED2 同步闪烁 */
    BSP_ApplyLeds((on != 0U) ? 0x03U : 0x00U);
}

uint8_t BSP_GetIrqLevel(void)
{
    /* PA3 上拉到高电平，ESC 拉低表示有待处理 AL Event */
    return (GPIO_ReadInputDataBit(E252_IRQ_GPIO_PORT, E252_IRQ_PIN) != 0U) ? 1U : 0U;
}

/* 在 ms 窗口内关闭 EXTI3（无人读 AL Event），用原始寄存器极速轮询 PA3：
 * 统计窗口内“引脚为低的总时长”与完整低脉冲个数。电平保持型 IRQ 会在首帧后
 * 一直为低（低电平总时长≈整个窗口）；短脉冲型只会累计几十 µs。
 * 结束后补一次软件中断，把积存的 AL Event 排空。 */
void BSP_MeasureIrqPulse(uint32_t ms, uint32_t *pulses, uint32_t *minNs,
                         uint32_t *avgNs, uint32_t *maxNs, uint32_t *lowUs,
                         uint8_t *endLow)
{
    uint32_t startMs;
    uint32_t elapsed = 0U;   /* 96 MHz tick 累计，循环内按 16 位增量叠加（TIM2 单圈 682 us） */
    uint32_t fallTick = 0U;
    uint32_t count = 0U;
    uint32_t sumTicks = 0U;
    uint32_t minTicks = 0xFFFFFFFFUL;
    uint32_t maxTicks = 0U;
    uint32_t lowTicks = 0U;
    uint32_t prevTick;
    uint8_t lastLevel;
    uint8_t level;

    NVIC_DisableIRQ(EXTI3_IRQn);
    EXTI_ClearITPendingBit(EXTI_Line3);

    lastLevel = ((GPIOA->INDR & E252_IRQ_PIN) != 0U) ? 1U : 0U;
    prevTick = TIM2->CNT;
    /* 窗口用 1 ms 软件 tick：TIM3 为 16 位 2 MHz，单次计数最多 32.7 ms，不能直接做 50 ms 门限 */
    startMs = BSP_Get1msTick();
    while ((uint32_t)(BSP_Get1msTick() - startMs) < ms)
    {
        uint32_t tick = TIM2->CNT;
        uint32_t delta = (uint16_t)(tick - prevTick);
        elapsed += delta;
        level = ((GPIOA->INDR & E252_IRQ_PIN) != 0U) ? 1U : 0U;
        if (level == 0U)
        {
            lowTicks += delta;
        }
        if ((lastLevel != 0U) && (level == 0U))
        {
            fallTick = elapsed;
        }
        else if ((lastLevel == 0U) && (level != 0U))
        {
            uint32_t width = (uint32_t)(elapsed - fallTick);
            sumTicks += width;
            if (width < minTicks)
            {
                minTicks = width;
            }
            if (width > maxTicks)
            {
                maxTicks = width;
            }
            count++;
        }
        prevTick = tick;
        lastLevel = level;
    }

    *endLow = lastLevel;
    NVIC_EnableIRQ(EXTI3_IRQn);
    EXTI_GenerateSWInterrupt(EXTI_Line3);

    *pulses = count;
    *lowUs = lowTicks / BOARD_TICKS_PER_US;   /* 1 us = BOARD_TICKS_PER_US 次轮询采样 */
    if (count != 0U)
    {
        *minNs = (minTicks * 125UL) / 12UL;
        *avgNs = ((sumTicks / count) * 125UL) / 12UL;
        *maxNs = (maxTicks * 125UL) / 12UL;
    }
    else
    {
        *minNs = 0U;
        *avgNs = 0U;
        *maxNs = 0U;
    }
}
