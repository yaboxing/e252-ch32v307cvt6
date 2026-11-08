/**
 * @file    board_config.h
 * @brief   CH32V307VCT6 + E252 DPORT 板级引脚与时钟配置
 *
 * 引脚依据：CH32V307VCT6-R1 PCB 网表、《CH32V30x 评估板说明书》；
 *           SPI/GPIO/EXTI 配置参考 CH32V307 EVT 例程。
 */

#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include "ch32v30x.h"

/* ------------------------------------------------------------------------- */
/* 时钟                                                                       */
/* ------------------------------------------------------------------------- */
/* 主频选择：HSE 8 MHz × PLL 倍频。
 *   96(×12，默认) 80(×10) 72(×9) 64(×8) 56(×7) 48(×6)
 * 对应 SPI1 /2 = 48 / 40 / 36 / 32 / 28 / 24 MHz。
 * HCLK = SYSCLK，PCLK2(APB2) = SYSCLK，PCLK1(APB1) = SYSCLK/2。
 * 其它取值需同步核对 Flash 等待周期（RM 3.3.3）与 USB/SDIO 等未用外设。 */
#define E252_SYSCLK_MHZ             96U
#define BOARD_SYSCLK_HZ             (E252_SYSCLK_MHZ * 1000000UL)
#define BOARD_PCLK1_HZ              (BOARD_SYSCLK_HZ / 2U)

/* ------------------------------------------------------------------------- */
/* E252 PDI（SPI 实例选择）                                                   */
/*                                                                           */
/* SPI1：APB2 96 MHz。E252 手册 §11.3 表11-3 的 fSCK 上限 80 MHz 是器件可接受 */
/* 的时钟上限；读操作另受表11-3 注3 的 30 MHz 限制，实际可用上限还取决于      */
/* tV（SCK 下降沿→MISO 有效）与半周期之比，见下方预分频说明。                 */
/* SPI1 使用默认映射：SCK=PA5 / MISO=PA6 / MOSI=PA7，无需 AFIO 重映射。       */
/* CS 为软件片选 PB12（不占 SPI1_NSS/PA4），PA4 作 SYNC0。                    */
/*                                                                           */
/* SPI2：PB13/14/15，APB1 /2 = 24 MHz；该配置下 SYNC1 应取 PA5（J6-6），      */
/* 与下方 PB11 定义互斥。                                                     */
/* ------------------------------------------------------------------------- */
#define E252_SPI_USE_SPI1           1

#if E252_SPI_USE_SPI1
#define E252_SPI                    SPI1
#define E252_SPI_CLK                RCC_APB2Periph_SPI1
#define E252_SPI_ON_APB2            1
#define E252_SPI_PIN_PORT           GPIOA
#define E252_SPI_SCK_PIN            GPIO_Pin_5
#define E252_SPI_MISO_PIN           GPIO_Pin_6
#define E252_SPI_MOSI_PIN           GPIO_Pin_7
#else
#define E252_SPI                    SPI2
#define E252_SPI_CLK                RCC_APB1Periph_SPI2
#define E252_SPI_ON_APB2            0
#define E252_SPI_PIN_PORT           GPIOB
#define E252_SPI_SCK_PIN            GPIO_Pin_13
#define E252_SPI_MISO_PIN           GPIO_Pin_14
#define E252_SPI_MOSI_PIN           GPIO_Pin_15
#endif

/* CS 固定为软件片选 PB12，与 SPI1/SPI2 实例无关 */
#define E252_SPI_GPIO_CLK           (RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO)
#define E252_SPI_CS_PORT            GPIOB
#define E252_SPI_CS_PIN             GPIO_Pin_12

/* 预分频（当前取 /4）：                                                     */
/*   /2 → SPI1 = 48 MHz（APB2 96 MHz）、SPI2 = 24 MHz（APB1 48 MHz）           */
/*   /4 → SPI1 = 24 MHz、SPI2 = 12 MHz                                        */
/* SPI1 读操作取 24 MHz：E252 读指令上限 30 MHz（§11.3 表11-3 注3），32 MHz    */
/* 已越限；tV ≤11.0 ns(30 pF)/≤9.0 ns(10 pF)，CPHA=0 上升沿采样只有 T/2，     */
/* 24 MHz 半周期 20.8 ns 满足，32 MHz 半周期 15.6 ns 不满足。48 MHz（/2）下    */
/* 读回整帧右移 1 位（BYTE_TEST=0xC3B2A190、ID_REV=0x49298080）；CS 建立时间  */
/* 不是限制因素（tCSS ≥5 ns 即可），4 种 CPOL/CPHA 组合与 HSRXEN 均不能补偿   */
/* tV。24 MHz 下单次 PDI 事务约 16 µs；SPI2 24 MHz 约 17 µs。                 */
/* SPI2 要得到 24 MHz 时预分频取 /2；当前 /4 在 SPI2 下为 12 MHz。            */
#define E252_SPI_PRESCALER          SPI_BaudRatePrescaler_4

/* SPI 高速读模式（RM §20.4.10，SPIx_HSCR.bit0=HSRXEN，0x24 偏移）。
 * 置 0 时 BR[2:0] 映射 /2,/4,/8,…（96 MHz → 48/24/12 MHz）；
 * 置 1 时 BR[2:0] 映射 /2../9（96 MHz → 48/32/24/19.2/16/13.7/12/10.67 MHz）。
 * HSRXEN2（bit2）要求 PCLK≥120 MHz，本板 PCLK2=96 MHz，不使用。
 * 本板 CH32V307VCT6 的 HSRXEN 为 W0（不满足 RM §20.4.10 的 lot number 条件）：
 * 置 1 后 BR=001 仍为 /4（事务耗时约 348 ns，与 24 MHz 一致），/3../9 档不可用。 */
#define E252_SPI_HSRXEN             0

/* 调试开关（正常固件保持 0）：
 *   E252_CS_ASSERT_GAP_US：CS 拉低后、首个 SCK 前插入的软件等待（µs），
 *       用于检查 SCS# 建立时间的影响；tCSS 达标值 ≥5 ns。
 *   E252_SPI_TEST_MODE：0=CPOL0/CPHA0（1Edge 采样=上升沿），
 *       1=CPOL0/CPHA1，2=CPOL1/CPHA0，3=CPOL1/CPHA1，
 *       用于切换 SPI 采样边沿/相位。 */
#define E252_CS_ASSERT_GAP_US       0U
#define E252_SPI_TEST_MODE          0U

/* PDI 事务使用 DMA 搬运（TX=DMA1_Channel5、RX=DMA1_Channel4）。
 * 置 0 时使用逐字节 STATR 轮询实现，PDI 事务约 28 µs。 */
#define E252_SPI_USE_DMA            1

/* PDI CSR 卡死诊断：1 = 在 MainInit 前执行“地址 × 访问宽度”矩阵并串口打印
 * 完成情况；正常固件保持 0。探针命中 ESC 不执行的命令后会让 CSR busy 保持
 * 置位，仅用于诊断，之后需重新烧录正常固件才能继续跑 EtherCAT。 */
#define E252_PDI_CSR_PROBE          0

/* Direct CSR busy 轮询方式：1 = 单字节读 0x0307（在 E252 上不可靠：4 字节命令
 * 完成且数据有效时仍可能报 busy，产生 ~200 ms 假超时）；
 * 0 = 32 位读 CSR_CMD（默认；合法命令一次轮询即完成）。 */
#define PDI_CSR_POLL_USE_BYTE       0

/* ------------------------------------------------------------------------- */
/* E252 控制信号                                                              */
/* ------------------------------------------------------------------------- */
#define E252_IRQ_GPIO_PORT          GPIOA
#define E252_IRQ_PIN                GPIO_Pin_3
#define E252_IRQ_PORTSOURCE         GPIO_PortSourceGPIOA
#define E252_IRQ_PINSOURCE          GPIO_PinSource3

#define E252_SYNC0_GPIO_PORT        GPIOA
#define E252_SYNC0_PIN              GPIO_Pin_4
#define E252_SYNC0_PORTSOURCE       GPIO_PortSourceGPIOA
#define E252_SYNC0_PINSOURCE        GPIO_PinSource4

/* SYNC1 使用 PB11（J7-2，并联 J4-3）；PA5 用作 SPI1_SCK。
 * 依据：RM Table 10-16，TIM2_RM=10 时 CH4=PB11（CH1/CH2 在 PA0/PA1，
 * LED PWM 不受影响）；PB11 也可作 EXTI11。 */
#define E252_SYNC1_GPIO_PORT        GPIOB
#define E252_SYNC1_PIN              GPIO_Pin_11
#define E252_SYNC1_PORTSOURCE       GPIO_PortSourceGPIOB
#define E252_SYNC1_PINSOURCE        GPIO_PinSource11

#define E252_RESET_GPIO_PORT        GPIOB
#define E252_RESET_PIN              GPIO_Pin_0

/* ------------------------------------------------------------------------- */
/* 用户指示灯 / 按键（CH32V307VCT6-R1 EVT，依据 PCB 网表）                    */
/*                                                                           */
/* LED1：蓝色 LED，阳极经 R2=1 kΩ 接 VCC(3.3 V)，阴极=网络 LED1 → J3-19；      */
/*       需跳线 J3-19 ↔ J3-28(PA0) 才与 MCU 相连；低有效（灌电流点亮）。       */
/* LED2：同构，R3=1 kΩ，阴极 → J3-21；跳线 J3-21 ↔ J3-30(PA1)；低有效。        */
/* KEY ：USER 按键 S2，脚 2/3=GND，脚 1=网络 KEY → J3-31；R24=10 kΩ 上拉 VCC； */
/*       跳线 J3-31 ↔ J3-32(PA2)；松开=高，按下=低。                           */
/* 依据：CH32V307VCT6-R1.kicad_pcb 网表；《CH32V30x 评估板说明书》p3 第 13/14 条 */
/* ------------------------------------------------------------------------- */
#define BOARD_LED1_PORT             GPIOA
#define BOARD_LED1_PIN              GPIO_Pin_0   /* J3-28, TIM2_CH1 */
#define BOARD_LED2_PORT             GPIOA
#define BOARD_LED2_PIN              GPIO_Pin_1   /* J3-30, TIM2_CH2 */
#define BOARD_USER_KEY_PORT         GPIOA
#define BOARD_USER_KEY_PIN          GPIO_Pin_2   /* J3-32 */
#define BOARD_USER_KEY_DEBOUNCE_MS  20U

/* ------------------------------------------------------------------------- */
/* 调试串口（USART1 -> WCH-LinkE 串口桥，EVT 板 PA9/PA10）                     */
/* ------------------------------------------------------------------------- */
#define BOARD_UART                  USART1
#define BOARD_UART_CLK              RCC_APB2Periph_USART1
#define BOARD_UART_GPIO_CLK         RCC_APB2Periph_GPIOA
#define BOARD_UART_GPIO_PORT        GPIOA
#define BOARD_UART_TX_PIN           GPIO_Pin_9
#define BOARD_UART_RX_PIN           GPIO_Pin_10
#define BOARD_UART_BAUDRATE         115200U

/* ------------------------------------------------------------------------- */
/* 定时器                                                                     */
/* ------------------------------------------------------------------------- */
/* CH32V307 的 TIM2~TIM4 挂在 APB1 上：APB1 预分频 != 1 时定时器时钟 = 2 x PCLK1。
 * PCLK1=48 MHz 时 TIM3/TIM4 为 96 MHz。
 * 依据：官方 EXAM 例程 TIM_INT 同样按 2 x PCLK1 配置。 */
#define BOARD_TIM_CLK_HZ            (BOARD_PCLK1_HZ * 2U)

/* TIM2 自由计数 1 tick = 1/SYSCLK（定时器时钟 = 2 x PCLK1 = SYSCLK）。
 * 所有 tick→µs 换算必须用这个宏；主频改变后若仍写死 96，latency/lock/byte_ns
 * 都会整体偏大或偏小。 */
#define BOARD_TICKS_PER_US          (BOARD_SYSCLK_HZ / 1000000UL)

/* TIM3: 2 MHz 自由计数，ECAT_TIMER_INC_P_MS = 2000 */
#define ECAT_TIMER_PSC              ((BOARD_TIM_CLK_HZ / 2000000U) - 1U)

/* TIM4: 1 ms 中断，驱动 ECAT_CheckTimer() */
#define ECAT_TIMER_1MS_PSC          (BOARD_TIM_CLK_HZ / 1000000U - 1U)
#define ECAT_TIMER_1MS_ARR          999U

/* ------------------------------------------------------------------------- */
/* 数字量 IO 影子映射（无外部 IO 扩展芯片）                                   */
/* ------------------------------------------------------------------------- */
#define BOARD_SWITCH_COUNT          8U

#endif /* _BOARD_CONFIG_H_ */
