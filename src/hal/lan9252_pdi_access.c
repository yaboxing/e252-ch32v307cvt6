#include "hal/lan9252_pdi_access.h"
#include "hal/e252_spi.h" /* PDI_PortReadN / 地址模式位 */
#include "ch32v30x.h"   /* TIM2：分段耗时诊断 */
#include "bsp/board_config.h" /* BOARD_TICKS_PER_US */
#include <limits.h>     /* CHAR_BIT */
#include <stdio.h>      /* PDI_CsrProbeRun 诊断输出 */

/* ESC CSR/PRAM 功能寄存器偏移（基址 0x300，地址显式展开便于静态核对） */
#define ECAT_REG_BASE_ADDR         0x0300U
#define CSR_DATA_REG               0x0300U  /* ECAT_REG_BASE_ADDR + 0x0000 */
#define CSR_CMD_REG                0x0304U  /* ECAT_REG_BASE_ADDR + 0x0004 */
#define PRAM_READ_ADDR_LEN_REG     0x0308U  /* ECAT_REG_BASE_ADDR + 0x0008 */
#define PRAM_READ_CMD_REG          0x030CU  /* ECAT_REG_BASE_ADDR + 0x000C */
#define PRAM_WRITE_ADDR_LEN_REG    0x0310U  /* ECAT_REG_BASE_ADDR + 0x0010 */
#define PRAM_WRITE_CMD_REG         0x0314U  /* ECAT_REG_BASE_ADDR + 0x0014 */
#define PRAM_READ_FIFO_REG         0x0004U
#define PRAM_WRITE_FIFO_REG        0x0020U

/* Indexed HBI 窗口寄存器偏移 */
#define HBI_INDEXED_INDEX0_REG     0x0000U
#define HBI_INDEXED_DATA0_REG      0x0004U
#define HBI_INDEXED_INDEX1_REG     0x0008U
#define HBI_INDEXED_DATA1_REG      0x000CU
#define HBI_INDEXED_INDEX2_REG     0x0010U
#define HBI_INDEXED_DATA2_REG      0x0014U
#define HBI_INDEXED_PRAM_FIFO_REG  0x0018U

/* PRAM FIFO 状态位 */
#define PRAM_SPACE_AVBL_COUNT_MASK 0x1FU
#define IS_PRAM_SPACE_AVBL_MASK    0x01U

/* CSR/PRAM 命令控制位 */
#define PRAM_RW_ABORT_MASK         (1UL << 30)
#define PRAM_RW_BUSY_32B           (1UL << 31)
#define PRAM_RW_BUSY_8B            0x80U
#define PRAM_SET_READ              (1U << 6)
#define PRAM_SET_WRITE             0U

/* CH32V307 port: TI C2000 "ramfuncs" CODE_SECTION pragmas removed (not supported by GCC). */

/* 按 EtherCAT 栈的 MEM_ADDR 宽度打包单字节 */
static void PDI_SetPackedByte(MEM_ADDR *buffer, UINT16 byteIndex, UINT16 value)
{
    /* MEM_ADDR 宽度（真实 8-bit 字节数），用于计算元素下标与字节偏移 */
    UINT16 memAddrBytes = (UINT16)((sizeof(MEM_ADDR) * (UINT16)CHAR_BIT) / 8U);
    UINT16 elementIndex = (UINT16)(byteIndex / memAddrBytes);
    UINT16 byteOffset = (UINT16)(byteIndex % memAddrBytes);
    UINT32 shift = (UINT32)byteOffset << 3;
    UINT32 mask = 0xFFUL << shift;
    UINT32 elementValue = (UINT32)buffer[elementIndex];

    elementValue = (elementValue & ~mask) | ((((UINT32)value) & 0xFFUL) << shift);
    buffer[elementIndex] = (MEM_ADDR)elementValue;
}

static UINT16 PDI_GetPackedByte(const MEM_ADDR *buffer, UINT16 byteIndex)
{
    /* 读取逻辑与写入逻辑对应，用于从按 MEM_ADDR 存放的缓冲区中取回单字节 */
    UINT16 memAddrBytes = (UINT16)((sizeof(MEM_ADDR) * (UINT16)CHAR_BIT) / 8U);
    UINT16 elementIndex = (UINT16)(byteIndex / memAddrBytes);
    UINT16 byteOffset = (UINT16)(byteIndex % memAddrBytes);
    UINT32 shift = (UINT32)byteOffset << 3;

    return (UINT16)((((UINT32)buffer[elementIndex]) >> shift) & 0xFFUL);
}

/* 从 32bit 值中取出指定字节 */
static UINT16 PDI_GetByte(UINT32 value, UINT16 byteIndex)
{
    return (UINT16)((value >> (byteIndex << 3)) & 0xFFU);
}

static void PDI_SetPackedDWord(MEM_ADDR *buffer, UINT16 byteIndex, UINT32 value)
{
    UINT16 memAddrBytes = (UINT16)((sizeof(MEM_ADDR) * (UINT16)CHAR_BIT) / 8U);

    if (((byteIndex & 0x3U) == 0U) && (memAddrBytes == 2U))
    {
        UINT16 elementIndex = (UINT16)(byteIndex >> 1);

        buffer[elementIndex] = (MEM_ADDR)(value & 0xFFFFUL);
        buffer[(UINT16)(elementIndex + 1U)] = (MEM_ADDR)((value >> 16) & 0xFFFFUL);
    }
    else
    {
        PDI_SetPackedByte(buffer, byteIndex, PDI_GetByte(value, 0U));
        PDI_SetPackedByte(buffer, (UINT16)(byteIndex + 1U), PDI_GetByte(value, 1U));
        PDI_SetPackedByte(buffer, (UINT16)(byteIndex + 2U), PDI_GetByte(value, 2U));
        PDI_SetPackedByte(buffer, (UINT16)(byteIndex + 3U), PDI_GetByte(value, 3U));
    }
}

static UINT32 PDI_GetPackedDWord(const MEM_ADDR *buffer, UINT16 byteIndex)
{
    UINT16 memAddrBytes = (UINT16)((sizeof(MEM_ADDR) * (UINT16)CHAR_BIT) / 8U);

    if (((byteIndex & 0x3U) == 0U) && (memAddrBytes == 2U))
    {
        UINT16 elementIndex = (UINT16)(byteIndex >> 1);

        return (((UINT32)buffer[elementIndex]) & 0xFFFFUL) |
               ((((UINT32)buffer[(UINT16)(elementIndex + 1U)]) & 0xFFFFUL) << 16);
    }

    return ((UINT32)PDI_GetPackedByte(buffer, byteIndex)) |
           (((UINT32)PDI_GetPackedByte(buffer, (UINT16)(byteIndex + 1U))) << 8) |
           (((UINT32)PDI_GetPackedByte(buffer, (UINT16)(byteIndex + 2U))) << 16) |
           (((UINT32)PDI_GetPackedByte(buffer, (UINT16)(byteIndex + 3U))) << 24);
}

/* CSR 访问保持 1/2/4 字节分段规则 */
static UINT16 PDI_GetCsrChunk(UINT16 address, UINT16 remaining)
{
    UINT16 chunk = remaining;

    if (chunk > 4U)
    {
        chunk = 4U;
    }

    if ((address & 0x1U) != 0U)
    {
        chunk = 1U;
    }
    else if ((address & 0x2U) != 0U)
    {
        chunk = ((chunk & 0x1U) != 0U) ? 1U : 2U;
    }
    else if (chunk == 3U)
    {
        chunk = 1U;
    }

    return chunk;
}

/* 等待 CSR 命令完成 */
static PDI_Status PDI_WaitForCsrReady(UINT32 timeout)
{
    UINT32 csrCmd;

    PDI_PortWrite32(HBI_INDEXED_INDEX0_REG, CSR_CMD_REG);
    do
    {
        csrCmd = PDI_PortRead32(HBI_INDEXED_DATA0_REG);
        if (timeout == 0U)
        {
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((csrCmd & PRAM_RW_BUSY_32B) != 0UL);

    return PDI_STATUS_OK;
}

/* 等待 PRAM 读 FIFO 就绪，并返回可读字数 */
static PDI_Status PDI_WaitForPramReadReady(UINT16 *spaceCount, UINT32 timeout)
{
    UINT32 cmdStatus;

    PDI_PortWrite32(HBI_INDEXED_INDEX1_REG, PRAM_READ_CMD_REG);
    do
    {
        cmdStatus = PDI_PortRead32(HBI_INDEXED_DATA1_REG);
        if (timeout == 0U)
        {
            *spaceCount = 0U;
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((cmdStatus & IS_PRAM_SPACE_AVBL_MASK) == 0UL);

    *spaceCount = (UINT16)((cmdStatus >> 8) & PRAM_SPACE_AVBL_COUNT_MASK);
    return PDI_STATUS_OK;
}

/* 等待 PRAM 写 FIFO 就绪，并返回可写字数 */
static PDI_Status PDI_WaitForPramWriteReady(UINT16 *spaceCount, UINT32 timeout)
{
    UINT32 cmdStatus;

    PDI_PortWrite32(HBI_INDEXED_INDEX1_REG, PRAM_WRITE_CMD_REG);
    do
    {
        cmdStatus = PDI_PortRead32(HBI_INDEXED_DATA1_REG);
        if (timeout == 0U)
        {
            *spaceCount = 0U;
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((cmdStatus & IS_PRAM_SPACE_AVBL_MASK) == 0UL);

    *spaceCount = (UINT16)((cmdStatus >> 8) & PRAM_SPACE_AVBL_COUNT_MASK);
    return PDI_STATUS_OK;
}

/* 等待 PRAM 读通道空闲 */
static PDI_Status PDI_WaitForPramReadIdle(UINT32 timeout)
{
    UINT32 cmdStatus;

    PDI_PortWrite32(HBI_INDEXED_INDEX1_REG, PRAM_READ_CMD_REG);
    do
    {
        cmdStatus = PDI_PortRead32(HBI_INDEXED_DATA1_REG);
        if (timeout == 0U)
        {
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((cmdStatus & PRAM_RW_BUSY_32B) != 0UL);

    return PDI_STATUS_OK;
}

/* 等待 PRAM 写通道空闲 */
static PDI_Status PDI_WaitForPramWriteIdle(UINT32 timeout)
{
    UINT32 cmdStatus;

    PDI_PortWrite32(HBI_INDEXED_INDEX1_REG, PRAM_WRITE_CMD_REG);
    do
    {
        cmdStatus = PDI_PortRead32(HBI_INDEXED_DATA1_REG);
        if (timeout == 0U)
        {
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((cmdStatus & PRAM_RW_BUSY_32B) != 0UL);

    return PDI_STATUS_OK;
}

/* 通过 CSR 访问 0x0000~0x0FFF 的 ESC 寄存器区 */
static PDI_Status PDI_ReadRegUsingCSR(MEM_ADDR *readBuffer, UINT16 bufferOffset, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 offset = 0U;

    (void)timeout;

    while (offset < count)
    {
        UINT16 chunk = PDI_GetCsrChunk((UINT16)(address + offset), (UINT16)(count - offset));
        UINT32 regData;
        UINT32 alignedAddr;
        UINT16 byteOffset;
        UINT16 i;

        /* E252 工作在 SPI 模式时，ESC 寄存器区 0x0000~0x0FFF 可直接寻址读取
         * （PDI_PortRead32 的 0x0B FASTREAD；0x0064/0x0050 可正常读回）。
         * HBI 索引寄存器（0x0000/0x0004/0x0008/0x000C）的写-读握手在 SPI 模式
         * 下不成立：每次读都要跑满 timeout 次轮询才返回，PDI 中断里读
         * AL Event(0x0220) 会将中断耗时拖到数秒，s_tick1ms 停在 1、TIM4
         * 长期得不到执行；因此按 4 字节对齐直接读。 */
        alignedAddr = (UINT32)(address + offset) & ~3UL;
        byteOffset = (UINT16)((address + offset) & 0x3U);
        regData = PDI_PortRead32((UINT16)alignedAddr);

        for (i = 0U; i < chunk; i++)
        {
            PDI_SetPackedByte(readBuffer,
                                  (UINT16)(bufferOffset + offset + i),
                                  PDI_GetByte(regData, (UINT16)(byteOffset + i)));
        }

        offset = (UINT16)(offset + chunk);
    }

    return PDI_STATUS_OK;
}

/* 通过 CSR 写 0x0000~0x0FFF 的 ESC 寄存器区 */
static PDI_Status PDI_WriteRegUsingCSR(const MEM_ADDR *writeBuffer, UINT16 bufferOffset, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 offset = 0U;

    while (offset < count)
    {
        UINT16 chunk = PDI_GetCsrChunk((UINT16)(address + offset), (UINT16)(count - offset));
        UINT32 csrData = 0U;
        UINT16 i;

        for (i = 0U; i < chunk; i++)
        {
            csrData |= ((UINT32)PDI_GetPackedByte(writeBuffer,
                                                      (UINT16)(bufferOffset + offset + i))) << (i << 3);
        }

        PDI_PortWrite32(HBI_INDEXED_INDEX0_REG, CSR_DATA_REG);
        PDI_PortWrite32(HBI_INDEXED_DATA0_REG, csrData);

        PDI_PortWrite32(HBI_INDEXED_INDEX0_REG, CSR_CMD_REG);
        PDI_PortWrite32(HBI_INDEXED_DATA0_REG,
                           (UINT32)(address + offset) |
                           ((UINT32)chunk << 16) |
                           ((UINT32)(PRAM_RW_BUSY_8B | PRAM_SET_WRITE) << 24));

        if (PDI_WaitForCsrReady(timeout) != PDI_STATUS_OK)
        {
            return PDI_STATUS_TIMEOUT;
        }

        offset = (UINT16)(offset + chunk);
    }

    return PDI_STATUS_OK;
}

/* 通过 PRAM FIFO 读取 0x1000 及以上的 Process RAM */
static PDI_Status PDI_ReadPDRamRegister(MEM_ADDR *readBuffer, UINT16 bufferOffset, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 offset = 0U;
    UINT16 fifoWordsAvailable = 0U;
    UINT16 firstByteOffset;
    UINT16 chunk;
    UINT32 fifoData;
    UINT16 i;

    /* 先终止上一次 PRAM 读事务，确保 FIFO/状态机回到空闲态 */
    PDI_PortWrite32(HBI_INDEXED_INDEX1_REG, PRAM_READ_CMD_REG);
    PDI_PortWrite32(HBI_INDEXED_DATA1_REG, PRAM_RW_ABORT_MASK);
    if (PDI_WaitForPramReadIdle(timeout) != PDI_STATUS_OK)
    {
        return PDI_STATUS_TIMEOUT;
    }

    PDI_PortWrite32(HBI_INDEXED_INDEX2_REG, PRAM_READ_ADDR_LEN_REG);
    PDI_PortWrite32(HBI_INDEXED_DATA2_REG, (UINT32)address | ((UINT32)count << 16));

    PDI_PortWrite32(HBI_INDEXED_INDEX1_REG, PRAM_READ_CMD_REG);
    PDI_PortWrite32(HBI_INDEXED_DATA1_REG, PRAM_RW_BUSY_32B);
    if (PDI_WaitForPramReadReady(&fifoWordsAvailable, timeout) != PDI_STATUS_OK)
    {
        return PDI_STATUS_TIMEOUT;
    }

    fifoData = PDI_PortRead32(HBI_INDEXED_PRAM_FIFO_REG);
    if (fifoWordsAvailable > 0U)
    {
        fifoWordsAvailable--;
    }

    /* 首个 FIFO 字可能从 32bit 字中的中间字节开始，需要按地址低 2 位做对齐修正 */
    firstByteOffset = (UINT16)(address & 0x3U);
    chunk = (UINT16)(4U - firstByteOffset);
    if (chunk > count)
    {
        chunk = count;
    }

    if ((chunk == 4U) && (((bufferOffset + offset) & 0x3U) == 0U))
    {
        PDI_SetPackedDWord(readBuffer, (UINT16)(bufferOffset + offset), fifoData);
        offset = (UINT16)(offset + 4U);
    }
    else
    {
        for (i = 0U; i < chunk; i++)
        {
            PDI_SetPackedByte(readBuffer,
                                  (UINT16)(bufferOffset + offset),
                                  PDI_GetByte(fifoData, (UINT16)(firstByteOffset + i)));
            offset++;
        }
    }

    while (offset < count)
    {
        /* 后续数据已经按 FIFO 字对齐，可按 4 字节块顺序搬运 */
        if (fifoWordsAvailable == 0U)
        {
            if (PDI_WaitForPramReadReady(&fifoWordsAvailable, timeout) != PDI_STATUS_OK)
            {
                return PDI_STATUS_TIMEOUT;
            }
        }

        fifoData = PDI_PortRead32(HBI_INDEXED_PRAM_FIFO_REG);
        fifoWordsAvailable--;

        chunk = (UINT16)(count - offset);
        if (chunk > 4U)
        {
            chunk = 4U;
        }

        if ((chunk == 4U) && (((bufferOffset + offset) & 0x3U) == 0U))
        {
            PDI_SetPackedDWord(readBuffer, (UINT16)(bufferOffset + offset), fifoData);
            offset = (UINT16)(offset + 4U);
        }
        else
        {
            for (i = 0U; i < chunk; i++)
            {
                PDI_SetPackedByte(readBuffer,
                                      (UINT16)(bufferOffset + offset),
                                      PDI_GetByte(fifoData, i));
                offset++;
            }
        }
    }

    return PDI_STATUS_OK;
}

/* 通过 PRAM FIFO 写 0x1000 及以上的 Process RAM */
static PDI_Status PDI_WritePDRamRegister(const MEM_ADDR *writeBuffer, UINT16 bufferOffset, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 offset = 0U;
    UINT16 fifoWordsAvailable = 0U;
    UINT16 firstByteOffset;
    UINT16 chunk;
    UINT32 fifoData = 0U;
    UINT16 i;

    /* 写流程与读流程对称，先中止旧事务，再配置地址/长度并启动新事务 */
    PDI_PortWrite32(HBI_INDEXED_INDEX1_REG, PRAM_WRITE_CMD_REG);
    PDI_PortWrite32(HBI_INDEXED_DATA1_REG, PRAM_RW_ABORT_MASK);
    if (PDI_WaitForPramWriteIdle(timeout) != PDI_STATUS_OK)
    {
        return PDI_STATUS_TIMEOUT;
    }

    PDI_PortWrite32(HBI_INDEXED_INDEX2_REG, PRAM_WRITE_ADDR_LEN_REG);
    PDI_PortWrite32(HBI_INDEXED_DATA2_REG, (UINT32)address | ((UINT32)count << 16));

    PDI_PortWrite32(HBI_INDEXED_INDEX1_REG, PRAM_WRITE_CMD_REG);
    PDI_PortWrite32(HBI_INDEXED_DATA1_REG, PRAM_RW_BUSY_32B);
    if (PDI_WaitForPramWriteReady(&fifoWordsAvailable, timeout) != PDI_STATUS_OK)
    {
        return PDI_STATUS_TIMEOUT;
    }

    firstByteOffset = (UINT16)(address & 0x3U);
    chunk = (UINT16)(4U - firstByteOffset);
    if (chunk > count)
    {
        chunk = count;
    }

    /* 非 4 字节对齐地址时，首个写入字要从目标字内部的偏移字节开始填充 */
    if ((chunk == 4U) && (((bufferOffset + offset) & 0x3U) == 0U))
    {
        fifoData = PDI_GetPackedDWord(writeBuffer, (UINT16)(bufferOffset + offset));
        offset = (UINT16)(offset + 4U);
    }
    else
    {
        for (i = 0U; i < chunk; i++)
        {
            fifoData |= ((UINT32)PDI_GetPackedByte(writeBuffer,
                                                       (UINT16)(bufferOffset + offset))) <<
                        ((UINT16)(firstByteOffset + i) << 3);
            offset++;
        }
    }

    PDI_PortWrite32(HBI_INDEXED_PRAM_FIFO_REG, fifoData);
    if (fifoWordsAvailable > 0U)
    {
        fifoWordsAvailable--;
    }

    while (offset < count)
    {
        if (fifoWordsAvailable == 0U)
        {
            if (PDI_WaitForPramWriteReady(&fifoWordsAvailable, timeout) != PDI_STATUS_OK)
            {
                return PDI_STATUS_TIMEOUT;
            }
        }

        fifoData = 0U;
        chunk = (UINT16)(count - offset);
        if (chunk > 4U)
        {
            chunk = 4U;
        }

        /* 对齐后续部分按完整 32bit 字持续写入 FIFO */
        if ((chunk == 4U) && (((bufferOffset + offset) & 0x3U) == 0U))
        {
            fifoData = PDI_GetPackedDWord(writeBuffer, (UINT16)(bufferOffset + offset));
            offset = (UINT16)(offset + 4U);
        }
        else
        {
            for (i = 0U; i < chunk; i++)
            {
                fifoData |= ((UINT32)PDI_GetPackedByte(writeBuffer,
                                                           (UINT16)(bufferOffset + offset))) << (i << 3);
                offset++;
            }
        }

        PDI_PortWrite32(HBI_INDEXED_PRAM_FIFO_REG, fifoData);
        fifoWordsAvailable--;
    }

    return PDI_STATUS_OK;
}

/* -------------------------------------------------------------------------- */
/* Direct 模式                                                                */
/* 直接使用 LAN9252 的 CSR/PRAM 寄存器地址，不经过 Indexed HBI 窗口           */
/* -------------------------------------------------------------------------- */

/* 观测：AL Event(0x0220) CSR 读的 busy 轮询次数（诊断用，不影响协议） */
static UINT32 s_csrPollSum;
static UINT32 s_csrPollMax;
static UINT32 s_csrPollCnt;
static UINT32 s_csrPollLast;

void PDI_TakeCsrPollStats(UINT32 *avg, UINT32 *max)
{
    *avg = (s_csrPollCnt != 0U) ? (s_csrPollSum / s_csrPollCnt) : 0U;
    *max = s_csrPollMax;
    s_csrPollSum = 0U;
    s_csrPollMax = 0U;
    s_csrPollCnt = 0U;
}

/* 分段耗时：把一次 AL Event(0x0220) CSR 读拆成
 *   命令写（CSR_CMD 7 字节） / 合并读（CSR_CMD+CSR_DATA 递减 8 字节） / 重试次数。
 * 用于判定 100+ µs 是 SPI 字节开销、忙等还是 ESC 内部执行延迟。 */
static UINT32 s_csrSegCmdTicks;
static UINT32 s_csrSegPollTicks;
static UINT32 s_csrSegReadTicks;
static UINT32 s_csrSegRetries;
static UINT32 s_csrSegN;

void PDI_TakeCsrSegStats(UINT32 *cmdUs, UINT32 *pollUs, UINT32 *dataUs, UINT32 *n)
{
    UINT32 cnt = s_csrSegN;

    *n = cnt;
    *cmdUs  = (cnt != 0U) ? ((s_csrSegCmdTicks  / cnt) / BOARD_TICKS_PER_US) : 0UL;
    *pollUs = (cnt != 0U) ? ((s_csrSegPollTicks / cnt) / BOARD_TICKS_PER_US) : 0UL;
    *dataUs = (cnt != 0U) ? ((s_csrSegReadTicks / cnt) / BOARD_TICKS_PER_US) : 0UL;

    s_csrSegCmdTicks = 0U;
    s_csrSegPollTicks = 0U;
    s_csrSegReadTicks = 0U;
    s_csrSegRetries = 0U;
    s_csrSegN = 0U;
}

/* 等待 Direct CSR 命令完成。
 *
 * 单次轮询约 1.7~2 µs（24 MHz SPI 读 5 字节 + CS 间隔），若直接把调用方的
 * PDI_TIMEOUT=100000 当轮询次数，最坏一次读要阻塞约 200 ms；连续读多个
 * “ESC 不响应”的地址，表现为整机输出停顿数秒（易被误判为卡死）。
 * 这里对 CSR 轮询加硬上限：正常 CSR 命令 1~2 次轮询即完成，4096 次≈8 ms
 * 已远大于需求；超时立即返回失败并计数，不再无限放大阻塞。 */
#define PDI_CSR_POLL_MAX   4096U

static UINT32 s_csrTimeoutCount;

UINT32 PDI_GetCsrTimeoutCount(void)
{
    return s_csrTimeoutCount;
}

static PDI_Status PDI_WaitForDirectCsrReady(UINT32 timeout)
{
    /* busy 是 CSR_CMD 的 bit31（byte3 bit7）。
     * 单字节读 0x0307 时读到的值与 32 位 CSR_CMD 不一致（同一命令可能出现
     * “轮询看到 busy 永不清”但 32 位读已清零、数据已就绪），会导致误超时并
     * 放大成整机停顿。因此统一用 32 位读（每轮 8 字节，仅多 ~0.5 µs），
     * 保证 busy 判定与命令寄存器一致。 */
    UINT32 cmd;
    UINT32 polls = 0U;
    const UINT32 busyMask = ((UINT32)PRAM_RW_BUSY_8B << 24);

    if (timeout > PDI_CSR_POLL_MAX)
    {
        timeout = PDI_CSR_POLL_MAX;
    }

    do
    {
#if PDI_CSR_POLL_USE_BYTE
        cmd = ((UINT32)PDI_PortRead8((UINT16)(CSR_CMD_REG + 3U))) << 24;
#else
        cmd = PDI_PortRead32(CSR_CMD_REG);
#endif
        polls++;
        if (timeout == 0U)
        {
            s_csrPollLast = polls;
            s_csrTimeoutCount++;
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((cmd & busyMask) != 0U);

    s_csrPollLast = polls;
    return PDI_STATUS_OK;
}

/* 等待 Direct PRAM 读 FIFO 就绪，并返回可读字数 */
static PDI_Status PDI_WaitForDirectPramReadReady(UINT16 *spaceCount, UINT32 timeout)
{
    UINT32 cmdStatus;

    do
    {
        cmdStatus = PDI_PortRead32(PRAM_READ_CMD_REG);
        if (timeout == 0U)
        {
            *spaceCount = 0U;
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((cmdStatus & IS_PRAM_SPACE_AVBL_MASK) == 0UL);

    *spaceCount = (UINT16)((cmdStatus >> 8) & PRAM_SPACE_AVBL_COUNT_MASK);
    return PDI_STATUS_OK;
}

/* 等待 Direct PRAM 写 FIFO 就绪，并返回可写字数 */
static PDI_Status PDI_WaitForDirectPramWriteReady(UINT16 *spaceCount, UINT32 timeout)
{
    UINT32 cmdStatus;

    do
    {
        cmdStatus = PDI_PortRead32(PRAM_WRITE_CMD_REG);
        if (timeout == 0U)
        {
            *spaceCount = 0U;
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((cmdStatus & IS_PRAM_SPACE_AVBL_MASK) == 0UL);

    *spaceCount = (UINT16)((cmdStatus >> 8) & PRAM_SPACE_AVBL_COUNT_MASK);
    return PDI_STATUS_OK;
}

/* 等待 Direct PRAM 读通道空闲 */
static PDI_Status PDI_WaitForDirectPramReadIdle(UINT32 timeout)
{
    UINT32 cmdStatus;

    do
    {
        cmdStatus = PDI_PortRead32(PRAM_READ_CMD_REG);
        if (timeout == 0U)
        {
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((((cmdStatus >> 24) & 0xFFUL) & PRAM_RW_BUSY_8B) != 0UL);

    return PDI_STATUS_OK;
}

/* 等待 Direct PRAM 写通道空闲 */
static PDI_Status PDI_WaitForDirectPramWriteIdle(UINT32 timeout)
{
    UINT32 cmdStatus;

    do
    {
        cmdStatus = PDI_PortRead32(PRAM_WRITE_CMD_REG);
        if (timeout == 0U)
        {
            return PDI_STATUS_TIMEOUT;
        }
        timeout--;
    } while ((((cmdStatus >> 24) & 0xFFUL) & PRAM_RW_BUSY_8B) != 0UL);

    return PDI_STATUS_OK;
}

/* 通过 Direct CSR 访问 0x0000~0x0FFF 的 ESC 寄存器区 */
static PDI_Status PDI_DirectReadRegUsingCSR(MEM_ADDR *readBuffer, UINT16 bufferOffset, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 offset = 0U;

    while (offset < count)
    {
        UINT16 chunk = PDI_GetCsrChunk((UINT16)(address + offset), (UINT16)(count - offset));
        UINT32 csrData;
        UINT16 i;
        UINT32 tA = 0U;
        UINT32 tB = 0U;
        UINT32 tC = 0U;
        UINT32 tD = 0U;
        UINT8 isAl = ((((UINT16)(address + offset)) == 0x0220U) && (chunk <= 2U)) ? 1U : 0U;

        if (isAl != 0U) { tA = TIM2->CNT; }

        PDI_PortWrite32(CSR_CMD_REG,
                            (UINT32)(address + offset) |
                            ((UINT32)chunk << 16) |
                            ((UINT32)(PRAM_RW_BUSY_8B | PRAM_SET_READ) << 24));

        if (isAl != 0U) { tB = TIM2->CNT; }

        if (PDI_WaitForDirectCsrReady(timeout) != PDI_STATUS_OK)
        {
            return PDI_STATUS_TIMEOUT;
        }
        if (isAl != 0U) { tC = TIM2->CNT; }
        csrData = PDI_PortRead32(CSR_DATA_REG);

        tD = TIM2->CNT;

        if (isAl != 0U)
        {
            s_csrSegCmdTicks  += (UINT32)(UINT16)(tB - tA);
            s_csrSegPollTicks += (UINT32)(UINT16)(tC - tB);
            s_csrSegReadTicks += (UINT32)(UINT16)(tD - tC);
            s_csrSegRetries   += s_csrPollLast;
            s_csrSegN++;

            s_csrPollSum += s_csrPollLast;
            s_csrPollCnt++;
            if (s_csrPollLast > s_csrPollMax)
            {
                s_csrPollMax = s_csrPollLast;
            }
        }

        for (i = 0U; i < chunk; i++)
        {
            PDI_SetPackedByte(readBuffer,
                                  (UINT16)(bufferOffset + offset + i),
                                  PDI_GetByte(csrData, i));
        }

        offset = (UINT16)(offset + chunk);
    }

    return PDI_STATUS_OK;
}

/* 通过 Direct CSR 写 0x0000~0x0FFF 的 ESC 寄存器区 */
static PDI_Status PDI_DirectWriteRegUsingCSR(const MEM_ADDR *writeBuffer, UINT16 bufferOffset, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 offset = 0U;

    while (offset < count)
    {
        UINT16 chunk = PDI_GetCsrChunk((UINT16)(address + offset), (UINT16)(count - offset));
        UINT32 csrData = 0U;
        UINT16 i;

        for (i = 0U; i < chunk; i++)
        {
            csrData |= ((UINT32)PDI_GetPackedByte(writeBuffer,
                                                      (UINT16)(bufferOffset + offset + i))) << (i << 3);
        }

        PDI_PortWrite32(CSR_DATA_REG, csrData);
        PDI_PortWrite32(CSR_CMD_REG,
                            (UINT32)(address + offset) |
                            ((UINT32)chunk << 16) |
                            ((UINT32)(PRAM_RW_BUSY_8B | PRAM_SET_WRITE) << 24));

        if (PDI_WaitForDirectCsrReady(timeout) != PDI_STATUS_OK)
        {
            return PDI_STATUS_TIMEOUT;
        }

        offset = (UINT16)(offset + chunk);
    }

    return PDI_STATUS_OK;
}

/* 通过 Direct PRAM FIFO 读取 0x1000 及以上的 Process RAM */
static PDI_Status PDI_DirectReadPDRamRegister(MEM_ADDR *readBuffer, UINT16 bufferOffset, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 offset = 0U;
    UINT16 fifoWordsAvailable = 0U;
    UINT16 firstByteOffset;
    UINT16 chunk;
    UINT32 fifoData;
    UINT16 i;

    /* Direct 模式与 Indexed 模式流程一致，只是寄存器访问方式不同 */
    PDI_PortWrite32(PRAM_READ_CMD_REG, PRAM_RW_ABORT_MASK);
    if (PDI_WaitForDirectPramReadIdle(timeout) != PDI_STATUS_OK)
    {
        return PDI_STATUS_TIMEOUT;
    }

    PDI_PortWrite32(PRAM_READ_ADDR_LEN_REG, (UINT32)address | ((UINT32)count << 16));
    PDI_PortWrite32(PRAM_READ_CMD_REG, PRAM_RW_BUSY_32B);
    if (PDI_WaitForDirectPramReadReady(&fifoWordsAvailable, timeout) != PDI_STATUS_OK)
    {
        return PDI_STATUS_TIMEOUT;
    }

    fifoData = PDI_PortRead32(PRAM_READ_FIFO_REG);
    if (fifoWordsAvailable > 0U)
    {
        fifoWordsAvailable--;
    }

    /* 首个 FIFO 字可能是非对齐起点，需要跳过前导无效字节 */
    firstByteOffset = (UINT16)(address & 0x3U);
    chunk = (UINT16)(4U - firstByteOffset);
    if (chunk > count)
    {
        chunk = count;
    }

    if ((chunk == 4U) && (((bufferOffset + offset) & 0x3U) == 0U))
    {
        PDI_SetPackedDWord(readBuffer, (UINT16)(bufferOffset + offset), fifoData);
        offset = (UINT16)(offset + 4U);
    }
    else
    {
        for (i = 0U; i < chunk; i++)
        {
            PDI_SetPackedByte(readBuffer,
                                  (UINT16)(bufferOffset + offset),
                                  PDI_GetByte(fifoData, (UINT16)(firstByteOffset + i)));
            offset++;
        }
    }

    while (offset < count)
    {
        if (fifoWordsAvailable == 0U)
        {
            if (PDI_WaitForDirectPramReadReady(&fifoWordsAvailable, timeout) != PDI_STATUS_OK)
            {
                return PDI_STATUS_TIMEOUT;
            }
        }

        fifoData = PDI_PortRead32(PRAM_READ_FIFO_REG);
        fifoWordsAvailable--;

        chunk = (UINT16)(count - offset);
        if (chunk > 4U)
        {
            chunk = 4U;
        }

        if ((chunk == 4U) && (((bufferOffset + offset) & 0x3U) == 0U))
        {
            PDI_SetPackedDWord(readBuffer, (UINT16)(bufferOffset + offset), fifoData);
            offset = (UINT16)(offset + 4U);
        }
        else
        {
            for (i = 0U; i < chunk; i++)
            {
                PDI_SetPackedByte(readBuffer,
                                      (UINT16)(bufferOffset + offset),
                                      PDI_GetByte(fifoData, i));
                offset++;
            }
        }
    }

    return PDI_STATUS_OK;
}

/* 通过 Direct PRAM FIFO 写 0x1000 及以上的 Process RAM */
static PDI_Status PDI_DirectWritePDRamRegister(const MEM_ADDR *writeBuffer, UINT16 bufferOffset, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 offset = 0U;
    UINT16 fifoWordsAvailable = 0U;
    UINT16 firstByteOffset;
    UINT16 chunk;
    UINT32 fifoData = 0U;
    UINT16 i;

    /* 先确保上一写事务结束，避免当前长度/地址配置与前一次状态冲突 */
    PDI_PortWrite32(PRAM_WRITE_CMD_REG, PRAM_RW_ABORT_MASK);
    if (PDI_WaitForDirectPramWriteIdle(timeout) != PDI_STATUS_OK)
    {
        return PDI_STATUS_TIMEOUT;
    }

    PDI_PortWrite32(PRAM_WRITE_ADDR_LEN_REG, (UINT32)address | ((UINT32)count << 16));
    PDI_PortWrite32(PRAM_WRITE_CMD_REG, PRAM_RW_BUSY_32B);
    if (PDI_WaitForDirectPramWriteReady(&fifoWordsAvailable, timeout) != PDI_STATUS_OK)
    {
        return PDI_STATUS_TIMEOUT;
    }

    firstByteOffset = (UINT16)(address & 0x3U);
    chunk = (UINT16)(4U - firstByteOffset);
    if (chunk > count)
    {
        chunk = count;
    }

    /* 首个写入字可能从 32bit 字内部偏移处开始填充 */
    if ((chunk == 4U) && (((bufferOffset + offset) & 0x3U) == 0U))
    {
        fifoData = PDI_GetPackedDWord(writeBuffer, (UINT16)(bufferOffset + offset));
        offset = (UINT16)(offset + 4U);
    }
    else
    {
        for (i = 0U; i < chunk; i++)
        {
            fifoData |= ((UINT32)PDI_GetPackedByte(writeBuffer,
                                                       (UINT16)(bufferOffset + offset))) <<
                        ((UINT16)(firstByteOffset + i) << 3);
            offset++;
        }
    }

    PDI_PortWrite32(PRAM_WRITE_FIFO_REG, fifoData);
    if (fifoWordsAvailable > 0U)
    {
        fifoWordsAvailable--;
    }

    while (offset < count)
    {
        if (fifoWordsAvailable == 0U)
        {
            if (PDI_WaitForDirectPramWriteReady(&fifoWordsAvailable, timeout) != PDI_STATUS_OK)
            {
                return PDI_STATUS_TIMEOUT;
            }
        }

        fifoData = 0U;
        chunk = (UINT16)(count - offset);
        if (chunk > 4U)
        {
            chunk = 4U;
        }

        if ((chunk == 4U) && (((bufferOffset + offset) & 0x3U) == 0U))
        {
            fifoData = PDI_GetPackedDWord(writeBuffer, (UINT16)(bufferOffset + offset));
            offset = (UINT16)(offset + 4U);
        }
        else
        {
            for (i = 0U; i < chunk; i++)
            {
                fifoData |= ((UINT32)PDI_GetPackedByte(writeBuffer,
                                                           (UINT16)(bufferOffset + offset))) << (i << 3);
                offset++;
            }
        }

        PDI_PortWrite32(PRAM_WRITE_FIFO_REG, fifoData);
        fifoWordsAvailable--;
    }

    return PDI_STATUS_OK;
}

/* Direct 读接口：内部按地址自动切换 CSR/PRAM
 *
 * E252 数据手册 §13.11：ESC 内核 CSR 0x0000~0x0FFF 只能经 CSR_DATA(0x300)
 * / CSR_CMD(0x304) 间接访问；过程 RAM 0x1000~0x1FFF 经 PRAM FIFO 访问。
 * 只有系统 CSR（ID_REV=0x050、BYTE_TEST=0x064 等 0x050~0x314）才可直接
 * 线性寻址；对 0x0000~0x1FFF 统一线性直读/直写只对系统 CSR 有效，
 * 访问 AL Event Mask(0x0204) 等 ESC 内核 CSR 时读写不生效（HW_Init 返回 2），
 * 必须经 CSR_CMD/CSR_DATA 间接访问。 */
PDI_Status PDI_Direct_EscRead(MEM_ADDR *readBuffer, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 firstPart;
    PDI_Status status;

    if ((readBuffer == 0) || (count == 0U))
    {
        return PDI_STATUS_INVALID_PARAM;
    }

    /* 完全落在 ESC 内核 CSR 区时，走 CSR 命令窗口 */
    if ((UINT32)address + (UINT32)count <= 0x1000UL)
    {
        return PDI_DirectReadRegUsingCSR(readBuffer, 0U, address, count, timeout);
    }

    /* 完全落在过程 RAM 区时，走 PRAM 读 FIFO */
    if (address >= 0x1000U)
    {
        if (((count & 0x3U) == 2U) && (count >= 66U))
        {
            /* 尾部 2 字节单独一次读，使前一段保持 4 字节节拍 */
            UINT16 firstChunk = (UINT16)(count - 2U);

            status = PDI_DirectReadPDRamRegister(readBuffer, 0U, address, firstChunk, timeout);
            if (status != PDI_STATUS_OK)
            {
                return status;
            }

            return PDI_DirectReadPDRamRegister(readBuffer,
                                               firstChunk,
                                               (UINT16)(address + firstChunk),
                                               2U, timeout);
        }

        return PDI_DirectReadPDRamRegister(readBuffer, 0U, address, count, timeout);
    }

    /* 跨越 0x1000 边界：前半段 CSR，后半段 PRAM */
    firstPart = (UINT16)(0x1000U - address);
    status = PDI_DirectReadRegUsingCSR(readBuffer, 0U, address, firstPart, timeout);
    if (status != PDI_STATUS_OK)
    {
        return status;
    }

    return PDI_DirectReadPDRamRegister(readBuffer, firstPart, 0x1000U,
                                       (UINT16)(count - firstPart), timeout);
}

/* -------------------------------------------------------------------------- */
/* 一次性 CSR 诊断（仅诊断构建调用，正常固件不调用）                             */
/*                                                                            */
/* 把“读某些地址后固件长时间无输出”拆成三条可判定证据：                        */
/*   1) 命令是否完成：轮询次数、超时后 CSR_CMD.bit31 是否仍为 busy；             */
/*   2) ESC 实际返回的数据（CSR_DATA），用于区分“返回 0”和“没返回”；             */
/*   3) 失败后能否恢复：再发一条已知正常的 0x0220 读。                          */
/* 手册 §13.11 表 13-14 允许 size=1 落在 CSR_ADDR[1:0]=01，因此若该命令超时，    */
/* 说明是 ESC 侧不执行，而不是主机发了非法命令。                                */
/* -------------------------------------------------------------------------- */
typedef struct
{
    const char *name;
    UINT16 addr;
    UINT8  size;
} PDI_CSR_PROBE_ITEM;

UINT8 PDI_CsrProbeRun(void)
{
    /* 按手册表 13-14 只发合法组合：size=4 要求 addr[1:0]=00；size=2 要求 00/10；
     * size=1 任意。最后一条“ILL_141_2B”故意违反（0x0141 是奇数地址 + size2），
     * 用于检查：非法命令会让 CSR busy 永不清零、并阻塞后续所有 ESC 访问。 */
    static const PDI_CSR_PROBE_ITEM items[] =
    {
        { "AL_EVENT_2B",   0x0220U, 2U },
        { "CFG141_1B",     0x0141U, 1U },
        { "CFG142_1B",     0x0142U, 1U },
        { "DC0980_4B",     0x0980U, 4U },
        { "DC0984_2B",     0x0984U, 2U },
        { "DC0982_2B",     0x0982U, 2U },
        { "WD0442_2B",     0x0442U, 2U },
        { "WD0443_1B",     0x0443U, 1U },
        { "DC098E_1B",     0x098EU, 1U },
        { "DC098F_1B",     0x098FU, 1U },
        { "DC0981_1B",     0x0981U, 1U },
        { "DC09A0_4B",     0x09A0U, 4U },
        { "DC09A2_2B",     0x09A2U, 2U },
        { "DC09A4_4B",     0x09A4U, 4U },
        { "DC09A1_1B",     0x09A1U, 1U },
        { "RECV_0220_2B",  0x0220U, 2U },
        { "ILL_141_2B",    0x0141U, 2U },
    };
    UINT16 i;

    printf("[CSRP] start: table13-14 allows size=1 at any CSR_ADDR[1:0]\r\n");

    for (i = 0U; i < (UINT16)(sizeof(items) / sizeof(items[0])); i++)
    {
        UINT32 t0 = TIM3->CNT;
        UINT32 t1;
        UINT32 cmd;
        UINT32 data;
        UINT8  busy;
        UINT32 dtUs;
        PDI_Status st;

        PDI_PortWrite32(CSR_CMD_REG,
                        (UINT32)items[i].addr |
                        ((UINT32)items[i].size << 16) |
                        ((UINT32)(PRAM_RW_BUSY_8B | PRAM_SET_READ) << 24));

        st = PDI_WaitForDirectCsrReady(500U);
        t1 = TIM3->CNT;
        dtUs = (UINT32)(UINT16)(t1 - t0) / 2U;   /* TIM3 = 2 MHz -> 0.5 us/tick */

        cmd  = PDI_PortRead32(CSR_CMD_REG);
        data = PDI_PortRead32(CSR_DATA_REG);
        busy = (UINT8)((cmd >> 31) & 1U);

        printf("[CSRP] %-12s a=0x%04X sz=%u st=%u polls=%lu busy=%u dt_us=%lu cmd=0x%08lX data=0x%08lX\r\n",
               items[i].name,
               (unsigned)items[i].addr,
               (unsigned)items[i].size,
               (unsigned)st,
               (unsigned long)s_csrPollLast,
               (unsigned)busy,
               (unsigned long)dtUs,
               (unsigned long)cmd,
               (unsigned long)data);

        if (st != PDI_STATUS_OK)
        {
            printf("[CSRP] STOP at %s: CSR busy never cleared (cmd=0x%08lX)\r\n",
                   items[i].name, (unsigned long)cmd);
            return 0U;
        }
    }

    printf("[CSRP] done: all commands completed\r\n");
    return 1U;
}

/* Direct 写接口：内部按地址自动切换 CSR/PRAM */
PDI_Status PDI_Direct_EscWrite(const MEM_ADDR *writeBuffer, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 firstPart;
    PDI_Status status;

    if ((writeBuffer == 0) || (count == 0U))
    {
        return PDI_STATUS_INVALID_PARAM;
    }

    if ((UINT32)address + (UINT32)count <= 0x1000UL)
    {
        return PDI_DirectWriteRegUsingCSR(writeBuffer, 0U, address, count, timeout);
    }

    if (address >= 0x1000U)
    {
        return PDI_DirectWritePDRamRegister(writeBuffer, 0U, address, count, timeout);
    }

    firstPart = (UINT16)(0x1000U - address);
    status = PDI_DirectWriteRegUsingCSR(writeBuffer, 0U, address, firstPart, timeout);
    if (status != PDI_STATUS_OK)
    {
        return status;
    }

    return PDI_DirectWritePDRamRegister(writeBuffer, firstPart, 0x1000U,
                                        (UINT16)(count - firstPart), timeout);
}
















/* -------------------------------------------------------------------------- */
/* Indexed 模式                                                               */
/* 先通过 INDEX 寄存器选择目标地址，再从 DATA 窗口访问对应 CSR/PRAM 寄存器      */
/* -------------------------------------------------------------------------- */

/* Indexed 读接口：内部按地址自动切换 CSR/PRAM */
PDI_Status PDI_Index_EscRead(MEM_ADDR *readBuffer, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 firstPart;
    PDI_Status status;

    /* Indexed 模式的地址切换逻辑与 Direct 模式完全一致，仅底层读写函数不同 */
    if ((readBuffer == 0) || (count == 0U))
    {
        return PDI_STATUS_INVALID_PARAM;
    }

    if ((UINT32)address + (UINT32)count <= 0x1000UL)
    {
        return PDI_ReadRegUsingCSR(readBuffer, 0U, address, count, timeout);
    }

    if (address >= 0x1000U)
    {
        if (((count & 0x3U) == 2U) && (count >= 66U))
        {
            /* 与 Direct 模式保持同样的拆分策略，避免长报文尾部 2 字节落在单独的非对齐节拍里 */
            UINT16 firstChunk = (UINT16)(count - 2U);
            PDI_Status status;

            status = PDI_ReadPDRamRegister(readBuffer, 0U, address, firstChunk, timeout);
            if (status != PDI_STATUS_OK)
            {
                return status;
            }

            return PDI_ReadPDRamRegister(readBuffer,
                                             firstChunk,
                                             (UINT16)(address + firstChunk),
                                             2U, timeout);
        }

        return PDI_ReadPDRamRegister(readBuffer, 0U, address, count, timeout);
    }

    firstPart = (UINT16)(0x1000U - address);
    status = PDI_ReadRegUsingCSR(readBuffer, 0U, address, firstPart, timeout);
    if (status != PDI_STATUS_OK)
    {
        return status;
    }

    return PDI_ReadPDRamRegister(readBuffer, firstPart, 0x1000U, (UINT16)(count - firstPart), timeout);
}

/* Indexed 写接口：内部按地址自动切换 CSR/PRAM */
PDI_Status PDI_Index_EscWrite(const MEM_ADDR *writeBuffer, UINT16 address, UINT16 count, UINT32 timeout)
{
    UINT16 firstPart;
    PDI_Status status;

    /* 写跨区时先写 CSR 区，再继续写 PRAM 区，保证地址连续 */
    if ((writeBuffer == 0) || (count == 0U))
    {
        return PDI_STATUS_INVALID_PARAM;
    }

    if ((UINT32)address + (UINT32)count <= 0x1000UL)
    {
        return PDI_WriteRegUsingCSR(writeBuffer, 0U, address, count, timeout);
    }

    if (address >= 0x1000U)
    {
        return PDI_WritePDRamRegister(writeBuffer, 0U, address, count, timeout);
    }

    firstPart = (UINT16)(0x1000U - address);
    status = PDI_WriteRegUsingCSR(writeBuffer, 0U, address, firstPart, timeout);
    if (status != PDI_STATUS_OK)
    {
        return status;
    }

    return PDI_WritePDRamRegister(writeBuffer, firstPart, 0x1000U, (UINT16)(count - firstPart), timeout);
}

