/**
 * @file    lan9252_pdi_access.h
 * @brief   LAN9252 PDI 访问库
 *
 * ===========================================================================
 * 概述
 * ===========================================================================
 * 本库封装了 LAN9252 EtherCAT 从站控制器的主机接口（HBI）访问逻辑，
 * 支持两种 PDI 模式：
 *   - Direct 模式  ：直接映射寄存器访问。
 *   - Indexed 模式 ：通过 INDEX/DATA 窗口间接访问（适用于地址线受限的 MCU）。
 *
 * 库只依赖两个硬件相关函数（须由用户提供平台实现）：
 *   PDI_PortRead32  —— 读取 LAN9252 32-bit 寄存器
 *   PDI_PortWrite32 —— 写入 LAN9252 32-bit 寄存器
 *
 * ===========================================================================
 * 缓冲区约定
 * ===========================================================================
 * 读/写接口的 buffer 参数类型为 MEM_ADDR*，定义来自 EtherCAT 栈的 ecat_def.h。
 * MEM_ADDR 在不同 MCU 上宽度可能不同（例如 TI C2000 DSP 上为 16-bit）。
 * 调用需保证 buffer 至少能容纳 count 字节的内容。
 *
 * ===========================================================================
 * 错误处理
 * ===========================================================================
 * 所有接口返回 PDI_Status 枚举：
 *   PDI_STATUS_OK            —— 操作成功完成。
 *   PDI_STATUS_INVALID_PARAM —— 传入了空指针或零字节数。
 *   PDI_STATUS_TIMEOUT       —— 等待硬件就绪超时，通常意味着总线异常。
 *
 */

#ifndef __PDI_PDI_ACCESS_H__
#define __PDI_PDI_ACCESS_H__

#include "ecat_def.h"   /* MEM_ADDR, UINT8, UINT16, UINT32 等基础类型定义 */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* 公共类型                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 库函数统一返回状态码。
 */
typedef enum
{
    PDI_STATUS_OK            = 0U, /**< 操作成功                   */
    PDI_STATUS_INVALID_PARAM = 1U, /**< 参数非法（空指针或零字节数）。   */
    PDI_STATUS_TIMEOUT       = 2U  /**< 等待硬件就绪超时，总线可能异常。 */
} PDI_Status;



/* -------------------------------------------------------------------------- */
/* 库提供的接口函数                                                           */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* Direct 模式                                                                */
/* -------------------------------------------------------------------------- */

/** 返回 Direct CSR 命令超时累计次数（只增计数，用于定位长时间阻塞）。 */
UINT32 PDI_GetCsrTimeoutCount(void);

/**
 * @brief 一次性 CSR 自检：按表逐地址/宽度发读命令并打印完成情况。
 *
 * 仅用于 PDI 卡死根因诊断（`E252_PDI_CSR_PROBE` 为 1 时在 MainInit 前调用）。
 * 每条命令超时后立即停止，避免在 ESC busy 未清时继续写 CSR_CMD。
 *
 * @return 1 全部命令完成；0 在某条命令上超时（CSR busy 未清）。
 */
UINT8 PDI_CsrProbeRun(void);

/**
 * @brief 通过 LAN9252 Direct 通道读取 ESC 数据。
 *
 * 直接使用 LAN9252 的 CSR/PRAM 寄存器地址访问，不经过 Indexed HBI 窗口。
 * 库会根据目标 ESC 地址范围自动选择正确的底层访问方式：
 *   - 地址 0x0000 ~ 0x0FFF：使用 CSR 方式。
 *   - 地址 0x1000 及以上：  使用 PRAM FIFO 方式。
 *   - 跨越边界：            分两段自动处理。
 *
 * @param[out] readBuffer 保存读取数据的缓冲区（MEM_ADDR 元素数组）。
 * @param[in]  address    ESC 起始地址，单位字节。
 * @param[in]  count      要读取的字节数，不得为 0。
 * @param[in]  timeout    超时循环计数器初始值。
 *
 * @return PDI_STATUS_OK            成功。
 * @return PDI_STATUS_INVALID_PARAM readBuffer 为 NULL 或 count 为 0。
 * @return PDI_STATUS_TIMEOUT       等待硬件就绪超时。
 */
PDI_Status PDI_Direct_EscRead(MEM_ADDR *readBuffer, UINT16 address, UINT16 count, UINT32 timeout);

/**
 * @brief 通过 LAN9252 Direct 通道写入 ESC 数据。
 *
 * 与 PDI_Direct_EscRead 对应，写入方向。
 * 地址路由规则相同（CSR / PRAM 自动分段）。
 *
 * @param[in] writeBuffer 待写入数据的缓冲区（MEM_ADDR 元素数组）。
 * @param[in] address     ESC 起始地址，单位字节。
 * @param[in] count       要写入的字节数，不得为 0。
 * @param[in] timeout     超时循环计数器初始值。
 *
 * @return PDI_STATUS_OK            成功。
 * @return PDI_STATUS_INVALID_PARAM writeBuffer 为 NULL 或 count 为 0。
 * @return PDI_STATUS_TIMEOUT       等待硬件就绪超时。
 */
PDI_Status PDI_Direct_EscWrite(const MEM_ADDR *writeBuffer, UINT16 address, UINT16 count, UINT32 timeout);

/* -------------------------------------------------------------------------- */
/* Indexed 模式                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 通过 LAN9252 Indexed 通道读取 ESC 数据。
 *
 * 先通过 INDEX 寄存器选择目标地址，再从 DATA 窗口读取数据。
 * 适用于主机地址线不足以直接映射所有寄存器的 MCU（如 TI C2000）。
 * 地址路由规则与 Direct 模式完全一致（CSR / PRAM 自动分段）。
 *
 * @param[out] readBuffer 保存读取数据的缓冲区（MEM_ADDR 元素数组）。
 * @param[in]  address    ESC 起始地址，单位字节。
 * @param[in]  count      要读取的字节数，不得为 0。
 * @param[in]  timeout    超时循环计数器初始值。
 *
 * @return PDI_STATUS_OK            成功。
 * @return PDI_STATUS_INVALID_PARAM readBuffer 为 NULL 或 count 为 0。
 * @return PDI_STATUS_TIMEOUT       等待硬件就绪超时。
 */
PDI_Status PDI_Index_EscRead(MEM_ADDR *readBuffer, UINT16 address, UINT16 count, UINT32 timeout);

/**
 * @brief 通过 LAN9252 Indexed 通道写入 ESC 数据。
 *
 * 与 PDI_Index_EscRead 对应，写入方向。
 *
 * @param[in] writeBuffer 待写入数据的缓冲区（MEM_ADDR 元素数组）。
 * @param[in] address     ESC 起始地址，单位字节。
 * @param[in] count       要写入的字节数，不得为 0。
 * @param[in] timeout     超时循环计数器初始值。
 *
 * @return PDI_STATUS_OK            成功。
 * @return PDI_STATUS_INVALID_PARAM writeBuffer 为 NULL 或 count 为 0。
 * @return PDI_STATUS_TIMEOUT       等待硬件就绪超时。
 */
PDI_Status PDI_Index_EscWrite(const MEM_ADDR *writeBuffer, UINT16 address, UINT16 count, UINT32 timeout);

/* -------------------------------------------------------------------------- */
/* 需要由用户（平台驱动）实现的函数                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief 【必须实现】从 LAN9252 主机总线读取一个 32-bit 寄存器值。
 *
 * 本函数由用户在所在平台的驱动文件中实现，库本身不提供。
 *
 * 实现约定：
 *   1. `address` 是 LAN9252 主机接口寄存器偏移地址（非 ESC 地址）。
 *   2. 返回值必须按 little-endian 字节序组包：
 *        bit7:0   对应寄存器最低地址字节
 *        bit31:24 对应寄存器最高地址字节
 *   3. 每次调用必须完成一次完整的 32-bit 主机接口读取事务（不可分包）。
 *
 * @param[in] address LAN9252 主机接口寄存器偏移地址。
 * @return    从 LAN9252 读到的 32-bit 寄存器值（little-endian）。
 */
UINT32 PDI_PortRead32(UINT16 address);

/**
 * @brief 【必须实现】读取 LAN9252 主机接口的 1 个字节。
 *
 * 用于 CSR 忙位轮询（只读 CSR_CMD 的 byte3），避免每次轮询传 4 字节。
 *
 * @param[in] address LAN9252 主机接口寄存器偏移地址。
 * @return    读到的 1 字节。
 */
UINT8 PDI_PortRead8(UINT16 address);

/**
 * @brief 取出并清零 AL Event(0x0220) CSR 读的 busy 轮询统计（诊断）。
 * @param[out] avg 每秒平均轮询次数。
 * @param[out] max 每秒最大轮询次数。
 */
void PDI_TakeCsrPollStats(UINT32 *avg, UINT32 *max);

/**
 * @brief 取出并清零 AL Event CSR 读的分段耗时统计（诊断）。
 * @param[out] cmdUs  命令写（CSR_CMD）平均耗时，µs
 * @param[out] pollUs 忙位轮询平均耗时，µs
 * @param[out] dataUs 数据读（CSR_DATA）平均耗时，µs
 * @param[out] n      当前窗口内的样本数
 */
void PDI_TakeCsrSegStats(UINT32 *cmdUs, UINT32 *pollUs, UINT32 *dataUs, UINT32 *n);

/**
 * @brief 【必须实现】向 LAN9252 主机总线写入一个 32-bit 寄存器值。
 *
 * 本函数由用户在所在平台的驱动文件中实现，库本身不提供。
 *
 * 实现约定：
 *   1. `address` 是 LAN9252 主机接口寄存器偏移地址（非 ESC 地址）。
 *   2. `value` 按 little-endian 字节序解释：
 *        bit7:0   写入寄存器最低地址字节
 *        bit31:24 写入寄存器最高地址字节
 *   3. 每次调用必须完成一次完整的 32-bit 主机接口写入事务（不可分包）。
 *
 * @param[in] address LAN9252 主机接口寄存器偏移地址。
 * @param[in] value   要写入的 32-bit 寄存器值（little-endian）。
 */
void PDI_PortWrite32(UINT16 address, UINT32 value);

/* ---------------------------------------------------------------------------
 * 仅供 TI C2000 DSP 项目参考（RAM 运行配置）
 *
 * 作用：将库内函数放到 "ramfuncs" 段，使其从 Flash 拷贝到 RAM 后执行，
 * 可提高执行速度或改善主机接口时序稳定性。
 *
 * 【特别注意 - 配合 .lib 使用的情形】：
 * 如果将 lan9252_pdi_access.c 预先编译为了静态库 (.lib)，
 * 那么在应用工程的头文件中取消这些 pragma 注释是完全**无效**的！
 * （因为代码分段发生在编译期）
 *
 * 若想要预编译的 .lib 运行在 RAM 中，请使用以下两种方法之一：
 *  方法A：编译 .lib 时，先在本源码中取消下方注释，然后再编译打包为 .lib。
 *  方法B：利用应用工程的链接器脚本（.cmd 配置文件），将整个 .lib
 *         或者对应的 .obj 的 .text 段强行重定向到 ramfuncs。
 *
 * ---------------------------------------------------------------------------
 * #pragma CODE_SECTION(PDI_Direct_EscRead,  "ramfuncs")
 * #pragma CODE_SECTION(PDI_Direct_EscWrite, "ramfuncs")
 * #pragma CODE_SECTION(PDI_Index_EscRead,   "ramfuncs")
 * #pragma CODE_SECTION(PDI_Index_EscWrite,  "ramfuncs")
 *
 * #pragma CODE_SECTION(PDI_SetPackedByte,                  "ramfuncs")
 * #pragma CODE_SECTION(PDI_GetPackedByte,                  "ramfuncs")
 * #pragma CODE_SECTION(PDI_GetByte,                        "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForCsrReady,                "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForPramReadReady,           "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForPramWriteReady,          "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForPramReadIdle,            "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForPramWriteIdle,           "ramfuncs")
 * #pragma CODE_SECTION(PDI_ReadRegUsingCSR,                "ramfuncs")
 * #pragma CODE_SECTION(PDI_WriteRegUsingCSR,               "ramfuncs")
 * #pragma CODE_SECTION(PDI_ReadPDRamRegister,              "ramfuncs")
 * #pragma CODE_SECTION(PDI_WritePDRamRegister,             "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForDirectCsrReady,          "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForDirectPramReadReady,     "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForDirectPramWriteReady,    "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForDirectPramReadIdle,      "ramfuncs")
 * #pragma CODE_SECTION(PDI_WaitForDirectPramWriteIdle,     "ramfuncs")
 * #pragma CODE_SECTION(PDI_DirectReadRegUsingCSR,          "ramfuncs")
 * #pragma CODE_SECTION(PDI_DirectWriteRegUsingCSR,         "ramfuncs")
 * #pragma CODE_SECTION(PDI_DirectReadPDRamRegister,        "ramfuncs")
 * #pragma CODE_SECTION(PDI_DirectWritePDRamRegister,       "ramfuncs")
 */

#ifdef __cplusplus
}
#endif

#endif /* __LAN9252_PDI_ACCESS_H__ */
