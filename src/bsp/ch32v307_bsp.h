/**
 * @file    ch32v307_bsp.h
 * @brief   CH32V307 + E252 DPORT 板级支持包接口
 */

#ifndef _CH32V307_BSP_H_
#define _CH32V307_BSP_H_

#include "ch32v30x.h"

#ifdef __cplusplus
extern "C" {
#endif

/* E252 上电自检结果 */
typedef enum
{
    BSP_E252_SELFTEST_OK = 0,
    BSP_E252_SELFTEST_BYTE_TEST_FAIL,
    BSP_E252_SELFTEST_ID_REV_FAIL,
    BSP_E252_SELFTEST_TIMEOUT
} BSP_E252SelfTest;

typedef struct
{
    BSP_E252SelfTest result;
    uint32_t byteTest;      /* 期望 0x87654321 */
    uint32_t idRev;         /* E252 ID_REV */
} BSP_E252Info;

/* 系统与板级初始化：RCC/GPIO/SPI2/EXTI/TIM3/TIM4/Q_RESET/LED */
void BSP_Init(void);

/* 读取上电自检结果 */
const BSP_E252Info *BSP_GetE252Info(void);

/* 数字输出：bit0=LED1(PA0/TIM2_CH1)、bit1=LED2(PA1/TIM2_CH2)，1=亮；其余位仅存影子 */
void BSP_ApplyLeds(uint8_t mask);
uint8_t BSP_GetLedShadow(void);

/* PWM 调光：ch 0=LED1、1=LED2；duty 0=灭、65535≈全亮（低有效 LED，内部反相） */
void BSP_SetLedPwm(uint8_t ch, uint16_t duty);

/* 呼吸演示：两灯互补斜坡，约 2 s 一个周期（由 APPL_Application 在 byte0 bit2 置位时调用） */
void BSP_BreatheLeds(void);

/* 数字输入：bit0 = USER 按键（1=按下，20 ms 去抖），其余位为影子 */
uint8_t BSP_ReadSwitches(void);

/* 按键 1 ms 采样/去抖，由 TIM4 中断调用 */
void BSP_KeyTick(void);

/* 模拟输出：影子寄存器（EVT 板未接 DAC/PWM 负载） */
void BSP_ApplyAnalog(int16_t analogValue, uint16_t pwmValue);

/* 1 ms 节拍计数（由 TIM4 中断递增） */
uint32_t BSP_Get1msTick(void);
void BSP_Inc1msTick(void);

/* 错误指示：板上无独立 ERR 灯，借用 LED1/LED2 同步闪烁（仅启动失败诊断用） */
void BSP_SetErrorLed(uint8_t on);

/* E252 IRQ 引脚（PA3）当前电平：0=低（ESC 请求中断中），1=高 */
uint8_t BSP_GetIrqLevel(void);

/* 在 ms 窗口内轮询 PA3 并统计低脉冲宽度（TIM2 96 MHz 时基，单位 ns） */
void BSP_MeasureIrqPulse(uint32_t ms, uint32_t *pulses, uint32_t *minNs,
                         uint32_t *avgNs, uint32_t *maxNs, uint32_t *lowUs,
                         uint8_t *endLow);

/* 调试串口（USART1，115200 8N1）；printf 经 _write 重定向到此 */
void BSP_UartInit(void);
void BSP_UartPutChar(char c);

#ifdef __cplusplus
}
#endif

#endif /* _CH32V307_BSP_H_ */
