/**
 * @file    e252_spi.h
 * @brief   E252 (LAN9252 兼容) SPI PDI 平台钩子
 *
 * 提供 PDI 库所需的 PDI_PortRead32 / PDI_PortWrite32，
 * 负责 SPI2 的 8 位传输、软件片选与事务间隔。
 */

#ifndef _E252_SPI_H_
#define _E252_SPI_H_

#include "ecat_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/* E252 SPI 指令（数据手册 §11 / LAN9252 兼容） */
#define E252_CMD_FAST_READ      0x0BU   /* FASTREAD，<= 80 MHz，需 1 个空周期 */
#define E252_CMD_WRITE          0x02U   /* WRITE，<= 30 MHz */

/* 地址 bit15:14：01=递增，10=递减，00=固定 */
#define E252_ADDR_MODE_INC      0x4000U
#define E252_ADDR_MODE_DEC      0x8000U
#define E252_ADDR_MODE_FIX      0x0000U
#define E252_ADDR_MASK          0x3FFFU

/* 初始化 SPI2 与相关 GPIO（由 BSP_Init 调用） */
void E252_SPI_Init(void);

/* PDI 库平台钩子（地址为 E252 主机接口寄存器偏移，小端组包） */
UINT32 PDI_PortRead32(UINT16 address);
UINT8 PDI_PortRead8(UINT16 address);

void PDI_PortWrite32(UINT16 address, UINT32 value);

/* 多事务 PDI 序列原子化：屏蔽 MIE（保留 MPIE），返回原 MIE 状态。
 * 8 字节 SM 描述等访问由多次 CSR 命令组成（写 CSR_CMD → 轮询 → 读 CSR_DATA），
 * 若被 1 kHz TIM4（读 0x0440）打断，CSR 命令/数据窗口会被抢占，读到错值。
 * 用法：lock = E252_PdiLock(); ...序列...; E252_PdiUnlock(lock); */
UINT32 E252_PdiLock(void);
void E252_PdiUnlock(UINT32 state);

/* 最近一次 PDI 原子序列（MIE 关闭窗口）的最大持续时间，TIM2 96 MHz 计数 */
UINT32 E252_GetPdiLockMaxTicks(void);

/* 诊断：单次 CS 事务内连续收发 nBytes 个 0xFF 的耗时（TIM2 96 MHz tick），
 * 不含任何 ESC 命令语义，用于把“SPI 字节开销”与“ESC 内部执行延迟”分离 */
UINT32 E252_SPI_RawBench(UINT32 nBytes);

/* 诊断：DMA 搬运等待超时累计次数（正常应恒为 0） */
UINT32 E252_SPI_GetDmaTimeouts(void);

#ifdef __cplusplus
}
#endif

#endif /* _E252_SPI_H_ */
