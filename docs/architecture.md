# 固件架构与二次开发指南

本文面向两类读者：

1. **把从站移植到另一颗 MCU**：需要替换哪些文件、保留哪些代码、最容易出错的地方；
2. **变更从站功能**：改 PDO、对象字典、SM、看门狗或诊断信息时，哪些文件必须同步修改。

文中的路径均为仓库相对路径。固件源码位于 `src/`，ESI 描述文件位于 `docs/esi/`。

---

## 1. 分层与职责边界

| 目录 / 文件 | 职责 | 不该放在这里的内容 |
|---|---|---|
| `src/app/main.c` | 启动流程、模式选择、诊断打印、1 Hz 统计 | SPI 寄存器操作、EtherCAT 协议逻辑 |
| `src/app/el9800appl.c` | 应用业务：PDO 映射、LED/按键/模拟量处理、设备 ID | MCU 外设初始化 |
| `src/bsp/board_config.h` | 板级引脚、SPI 实例、时钟与功能宏 | 协议栈状态机 |
| `src/bsp/ch32v307_bsp.c` | 时钟、GPIO、SPI1/DMA、EXTI、TIM、LED/按键、上电自检 | 对象字典、PDO 业务 |
| `src/hal/e252_spi.c` 与 `src/hal/e252_spi.h` | E252 的 SPI 物理访问：片选、DMA、字节序、PDI 临界区 | EtherCAT 状态机 |
| `src/hal/lan9252_pdi_access.c` 与 `src/hal/lan9252_pdi_access.h` | PDI 事务模型：CSR/PRAM 窗口、忙轮询、尺寸与对齐策略 | MCU 寄存器定义 |
| `src/Ethercat/port/el9800hw.c` 与 `src/Ethercat/port/el9800hw.h` | 协议栈与硬件的适配层：`HW_Init`、`HW_EscRead/Write`、中断入口、1 ms 节拍、诊断结构 | 应用业务 |
| `src/Ethercat/src/*.c` | SSC 5.11 协议栈：状态机、邮箱、CoE/SDO、PDO 处理、看门狗、诊断 | MCU 外设访问 |
| `src/Ethercat/Inc/el9800appl.h` | 对象字典与 PDO 映射表、PDO 尺寸、应用数据结构 | 硬件驱动 |
| `src/Ethercat/Inc/ecat_def.h` | 协议栈裁剪宏、PD/邮箱最大尺寸 | 板级引脚 |
| `src/sdk/` | WCH SDK：启动文件、系统时钟、外设库、链接脚本 | 业务逻辑 |
| `src/Makefile` | 命令行构建 | 主站工具配置 |
| `src/tools/flash_stub/` | 烧录辅助程序 | 固件功能 |
| `docs/esi/E252_CH32V307.xml` | 对外发布的 ESI，主站导入用 | 运行时代码 |

分层的硬约束：

- **app 不直接碰 SPI/GPIO 寄存器**。所有板级操作通过 `BSP_*`，所有 ESC 访问通过 `HW_Esc*`。
- **hal 不理解 EtherCAT 语义**。它只知道“读/写某个 ESC 地址、多少字节、什么宽度”。
- **port 层是唯一允许同时包含 SSC 头文件与 MCU 头文件的地方**。
- **协议栈不包含任何 CH32 头文件**；它只通过 `port` 层与硬件交互。

---

## 2. 启动与运行主链路

### 2.1 启动顺序

```
复位
 └─ src/sdk/Startup/startup_ch32v30x_D8C.S
     └─ main()                                   src/app/main.c
         ├─ SystemCoreClockUpdate()              src/sdk/Core/system_ch32v30x.c
         ├─ Delay_Init()
         ├─ HW_Init()                            src/Ethercat/port/el9800hw.c
         │   ├─ BSP_Init()                       src/bsp/ch32v307_bsp.c
         │   │   ├─ 96 MHz 时钟 / GPIO / SPI1 + DMA
         │   │   ├─ EXTI3(IRQ)、EXTI4(SYNC0)、EXTI11(SYNC1)
         │   │   ├─ TIM2/TIM3 时基、TIM4 1 ms 节拍
         │   │   ├─ E252 复位、LED/按键初始化
         │   │   └─ 上电自检：BYTE_TEST、ID_REV、PDI 回环 0x1F00
         │   ├─ 检查自检结果，失败则停在错误态
         │   ├─ 配置 AL Event Mask、IRQ 配置（0x0054/0x005C）
         │   └─ 初始化诊断结构与 1 ms 节拍
         ├─ MainInit()                           src/Ethercat/src/ecatappl.c
         │   ├─ ECAT_Init()：对象字典、SM/FMMU、状态机
         │   ├─ 读取 SII 中的 SM 配置与 PDO 长度
         │   └─ 启动首个周期计时
         └─ while (1)
             ├─ MainLoop()                        src/Ethercat/src/ecatappl.c
             ├─ 1 Hz 诊断打印                    src/app/main.c
             └─ 按键/LED 业务                     src/app/el9800appl.c + BSP
```

关键点：

- `HW_Init()` 内部调用 `BSP_Init()`，因此 **MCU 时钟与外设必须先于协议栈初始化**。
- 自检失败时 `HW_Init()` 返回非 0，`main()` 不会进入协议栈主循环，便于用串口定位硬件问题。
- ESI 不参与启动；启动时协议栈读取的是板上 EEPROM 中的 SII。ESI 只用于主站组态。

### 2.2 周期内谁在跑

| 执行体 | 触发方式 | 负责 |
|---|---|---|
| 主循环 | `while(1)` | 非周期任务：邮箱/CoE、状态机推进、诊断、应用慢任务 |
| `TIM4` 中断 | 1 ms | 递增 `BSP_Get1msTick()`，供 `ECAT_CheckTimer()` 与看门狗统计 |
| `EXTI3` 中断 | E252 IRQ 下降沿 | **周期过程数据**：读 AL Event、输出映射、输入映射 |
| `EXTI4` / `EXTI11` | SYNC0 / SYNC1 边沿 | DC 同步事件、边沿时间戳 |
| `PDI_Isr()` | 由 `EXTI3` 调用 | 解析 AL Event，执行 PDO 映射与应用回调 |

两种运行模式：

- **FreeRun（无 SM 中断）**：主循环轮询 AL Event `0x0220`，发现事件后执行 `PDO_OutputMapping → ECAT_Application → PDO_InputMapping`。此时 EXTI3 关闭。
- **SM 同步（中断驱动）**：EXTI3 打开，`PDI_Isr()` 在中断里完成上述映射；主循环不再消费 AL Event。

`main.c` 负责模式切换：切到 SM 同步前必须清 EXTI pending 并软件触发一次，否则第一个输出事件丢失，主站进 OP 后会直接触发 SM 看门狗。

### 2.3 中断清序（最容易被忽略）

`EXTI3` 的尾部顺序固定为：

1. 读 `INT_STS`（`0x0058`），确认 E252 中断源；
2. 清除对应的 READY 位；
3. **等待 PA3 回到高电平**（E252 中断是低有效、电平保持）；
4. 清除 EXTI pending；
5. 退出中断。

若在 PA3 仍为低时就清 pending，中断会立刻再次进入，表现为“中断风暴”，主循环与 1 ms 节拍被饿死。SYNC0/SYNC1 的 `EXTI4`/`EXTI11` 只做“记录边沿 + 清 pending”，不要在中断里做耗时业务。

### 2.4 PDI 临界区

`E252_PdiLock()` / `E252_PdiUnlock()` 通过关闭 E252 的 AL Event 中断（`0x0300` 的 MIE 位）来保证一次多字节事务不被 IRQ 打断。它保护的是 **ESC 事务的原子性**，不是 MCU 全局中断。

移植时注意：

- 不要用“长时间关全局中断”替代该锁；会破坏 1 ms 节拍与串口。
- 锁的粒度应覆盖“命令 + 忙轮询 + 数据”，不能只包住数据段。
- 事务失败要保证解锁（超时路径必须成对调用）。

---

## 3. 数据流

### 3.1 输出（主站 → 从站）

```
主站周期帧
  → E252 端口0/1 收帧
  → ESC 写入 SM2（0x1200，6 B）
  → IRQ(AL Event)
  → EXTI3 → PDI_Isr
  → HW_EscReadIsr(0x1200, 6)
  → PDI_Direct_EscRead（CSR/PRAM 窗口 + 忙轮询）
  → APPL_OutputMapping → sDOOutputs / sAOOutputs
  → APPL_Application → BSP_ApplyLeds / BSP_ApplyAnalog
```

- `APPL_OutputMapping` 只做“字节/位 → 应用影子变量”的解包；
- `APPL_Application` 才把影子变量落到 GPIO/PWM；
- 在 OP 之前不要执行输出，否则 SAFEOP 的安全态会被破坏。

### 3.2 输入（从站 → 主站）

```
BSP_ReadSwitches / 模拟量采样
  → sDIInputs / sAIInputs（应用影子）
  → APPL_Application（主循环或 PDI_Isr）
  → APPL_InputMapping（PDI_Isr 内调用）
  → HW_EscWriteIsr(0x1300, 6)
  → SM3
  → 主站下一帧读回
```

**TXPDO 影子副本的原子性**：

- 当前 8 路 DI 打包在 16 位字内，写入是单次 16 位操作，天然原子；
- 若扩展到 32/64 位或多字节输入，`APPL_Application` 与 `APPL_InputMapping` 可能分别运行在主循环与中断，必须使用“影子缓冲 + 序号/双缓冲”或短暂临界区，避免主站读到半新半旧的数据；
- 诊断副本（`g_DiagPdInMap`）只用于观测，不能当作业务输入源。

### 3.3 邮箱、看门狗、DC 的位置

| 链路 | 路径 | 说明 |
|---|---|---|
| 邮箱 | SM0 `0x1000` / SM1 `0x1100` → `PDI_Isr`/`ECAT_Main` → `COE_Main` | SDO、对象字典读写；邮箱事件不参与周期 PDO |
| 看门狗 | ESC 内部 SM 看门狗 → `ECAT_CheckTimer()`（1 ms）→ `ecatslv.c` 状态处理 | 超时置 `AL=0x0014`、`CODE=0x001B`，输出进安全态 |
| DC/SYNC | SYNC0/1 引脚 → EXTI4/EXTI11 → `Sync0_Isr`/`Sync1_Isr` → 应用同步 | SYNC1 是 SYNC0 的相位偏移，不是独立周期 |
| 诊断 | `el9800hw.c` 统计 → `main.c` 1 Hz 打印 | 包含 IRQ 延迟、PDI 锁时长、看门狗、DC 计数 |

看门狗默认值来自 SII（`0x0400/0x0420`），协议栈启动时由 ESC 装载；固件不需要自己写。若产品需要不同默认窗口，改 SII/ESI，而不是在 `main()` 里硬编码。

---

## 4. 移植到其他 MCU：最小替换集合

### 4.1 必须替换

| # | 替换项 | 涉及文件 | 要实现的契约 |
|---|---|---|---|
| P1 | SPI/PDI 物理访问 | `src/hal/e252_spi.c` / `src/hal/e252_spi.h` | CS 拉低 → 命令 → 忙轮询 → 数据 → CS 释放；Mode 0；字节序正确 |
| P2 | PDI 端口回调 | `src/hal/lan9252_pdi_access.c` / `src/hal/lan9252_pdi_access.h` | `PDI_PortRead32`、`PDI_PortRead8`、`PDI_PortWrite32`、`PDI_PortWrite8` |
| P3 | `HW_EscRead/Write` 及 ISR 版本 | `src/Ethercat/port/el9800hw.c` | 普通版可重入；ISR 版不加锁，依赖中断屏蔽；失败必须有超时 |
| P4 | IRQ 与中断清除 | `src/Ethercat/port/el9800hw.c` | 低有效 IRQ、EXTI 配置、`INT_STS` 清序、等待 IRQ 释放 |
| P5 | 板级引脚 | `src/bsp/board_config.h`、`src/bsp/ch32v307_bsp.c` | SPI/CS/IRQ/SYNC0/SYNC1/RST/LED/KEY/串口 |
| P6 | 时钟与 1 ms 节拍 | `src/bsp/ch32v307_bsp.c` | `BSP_Get1msTick()`、`BSP_Inc1msTick()`、延时函数 |
| P7 | 启动文件与链接脚本 | `src/sdk/Startup/`、`src/sdk/Ld/` | 中断向量表、堆栈、复位入口 |
| P8 | 串口诊断 | `src/bsp/ch32v307_bsp.c`、`src/app/main.c` | 115200 输出，至少保留启动自检与错误行 |

### 4.2 可以原样复用

- `src/Ethercat/src/*.c` 与 `src/Ethercat/Inc/*.h` 的 SSC 协议栈（状态机、邮箱、CoE、PDO、看门狗、诊断）；
- `src/hal/lan9252_pdi_access.c` 的事务状态机与尺寸/对齐策略（只换回调）；
- `src/app/el9800appl.c` 的业务逻辑与 PDO 解包/打包框架；
- `src/Ethercat/Inc/el9800appl.h` 的对象字典与 PDO 表结构。

### 4.3 最容易踩的坑

1. **中断屏蔽与 PDI 原子性**
   - 多字节 CSR/PRAM 事务期间必须挡住 ESC 中断，否则同一对 CSR 窗口会被 ISR 重入，读到错数据；
   - 但不要关闭 EXTI3 太久：E252 IRQ 是电平保持，关太久会在释放瞬间积压事件；
   - 正确做法是“事务级锁 + 有界超时”，不是“全局关中断”。

2. **ISR 清序**
   - AL Event `0x0220` 是只读镜像，不能靠写它清零；
   - 清事件要读对应 SM 缓冲的首/末字（或按 ESC 手册规定的应答读）；
   - 先清 `INT_STS`，再等 IRQ 高，最后清 EXTI pending；顺序颠倒会中断风暴。

3. **片选与位时序**
   - CS 必须覆盖“命令 + 忙轮询 + 数据”全过程；中途抬高 CS 会让 ESC 复位事务状态；
   - CS 释放要等 SPI 的 BSY 清零并留出建立/保持间隙；
   - E252 读规格上限 30 MHz，飞线条件建议 24 MHz；48 MHz 在 CPHA=0 下采样窗口不足。

4. **尺寸与对齐**
   - `size=1` 任意地址可用；`size=2` 奇地址、`size=4` 非 4 字节对齐会卡死；
   - 端口层必须先做白名单校验，再发事务；
   - CSR 忙轮询只读 `CSR_CMD` 的 byte3，不要每次读 4 字节。

5. **DMA 通道与外设映射**
   - SPI1 与 SPI2 的 DMA 请求通道不同，换 MCU 后不能照抄通道号；
   - 事务结束条件要同时满足“TX 完成 + SPI BSY=0”，否则 CS 早释放会截断最后一个字节。

6. **FreeRun / SM 切换**
   - FreeRun 下 EXTI3 必须关闭，否则中断与轮询双消费 AL Event；
   - 切 SM 同步前清 pending、软件触发一次，否则丢首帧。

7. **LED/按键极性**
   - 本板 LED 为低电平点亮，按键按下为低；移植到高有效板卡时只改 BSP，不要改应用层逻辑。

8. **对象字典与 SII 的一致性**
   - 换 MCU 不改身份时，ESI/SII 可沿用；一旦改 PDO 长度或 SM 地址，必须同步重生成。

---

## 5. 变更从站功能：改动清单与联动

### 5.1 改 PDO 长度 / 映射

必须同步修改 **五处**（前四处是常说的“OD/SII/ESI/主站”，第五处是应用映射代码）：

| # | 位置 | 改什么 |
|---|---|---|
| F1 | `src/Ethercat/Inc/el9800appl.h` | `sDORxPDOMap`（`0x1600/0x1601/0x1602`）、`sDOTxPDOMap`（`0x1A00/0x1A02`）、`0x1C12/0x1C13` 分配对象、PDO 参数对象 |
| F2 | `src/app/el9800appl.c` | `APPL_GenerateMapping()` 的位/字节计算、`APPL_InputMapping`/`APPL_OutputMapping` 的拷贝长度、`APPL_Application` 的影子变量 |
| F3 | `docs/esi/E252_CH32V307.xml` | `Sm` 的 `DefaultSize/StartAddress/ControlByte`、`RxPdo/TxPdo` 条目、`Fmmu`、`0x1C12/0x1C13` 默认值 |
| F4 | EEPROM 中的 SII | 由 ESI 生成并写入；SM2/SM3 长度、控制字、FMMU、PDO 分配、身份 `0x1018` |
| F5 | 主站 IOmap / ENI | 过程数据长度、偏移、变量类型；ENI 必须重新生成 |

联动检查：

- `src/Ethercat/Inc/ecat_def.h` 的 `MAX_PD_INPUT_SIZE` / `MAX_PD_OUTPUT_SIZE`（当前 `0x44`）必须大于新长度；
- `src/Ethercat/src/ecatslv.c` 会在 PREOP→SAFEOP 时校验 SM 长度与生成尺寸，不匹配会报 `0x001D`（SM2）或 `0x001E`（SM3）；
- PDO 总长度超过 SM 窗口（例如 ≥129 B）会报 `0x0017`；
- 改完先用 `slaveinfo` 确认 SII 的 SM 长度，再进 OP。

### 5.2 改 SM 配置

| 项 | 位置 | 注意 |
|---|---|---|
| SM 地址/长度/控制字 | SII（由 ESI 生成） | 固件不在代码里硬编码；`ecatslv.c` 只校验 |
| SM2/SM3 缓冲模式 | SII 的 `ControlByte` | 过程数据用三缓冲；单缓冲 `01` 非法 |
| AL Event Mask | `HW_Init()`（`el9800hw.c`） | 改邮箱/PDO 事件类型时同步 |
| 邮箱尺寸 | SII + `ecat_def.h` | SM0/SM1 各 128 B |
| FMMU | ESI/SII | 地址/长度/方向必须与 PDO 映射一致 |

### 5.3 改看门狗时间

| 场景 | 改哪里 |
|---|---|
| 产品默认窗口 | SII 的 `0x0400`（分频）与 `0x0420`（100 µs 单位） |
| 运行时由主站改 | 主站写 `0x0420`；固件不改代码 |
| 固件兜底 | `ECAT_CheckTimer()` 只做节拍与统计，不覆盖 ESC 配置 |

注意：窗口必须大于主站实际帧周期的 2–5 倍；非实时主站建议 ≥10 ms。

### 5.4 改 TXPDO 影子副本

- 影子变量在 `src/Ethercat/Inc/el9800appl.h`（`sDIInputs`、`sAIInputs`）与 `src/app/el9800appl.c` 的 `APPL_Application` 中更新；
- `APPL_InputMapping` 运行在 `PDI_Isr` 内，必须保证读到的影子是一致快照；
- 8/16 位单字可原子读写；32 位以上或多字段结构建议：
  - 双缓冲 + 有效序号，ISR 只读“已完成”的那一份；
  - 或 `DISABLE_ESC_INT` / `ENABLE_ESC_INT` 包住快照拷贝（时间必须短）；
- 不要直接让 ISR 读主循环正在写的业务结构。

### 5.5 改诊断信息

| 项 | 位置 | 说明 |
|---|---|---|
| 诊断字段 | `src/Ethercat/Inc/el9800hw.h` 的 `HW_DIAG` | 新增字段要同步初始化与清零 |
| 统计采集 | `src/Ethercat/port/el9800hw.c`（IRQ/PDI/看门狗/SYNC） | 只在中断里做轻量累加，不打印 |
| 输出格式 | `src/app/main.c` 的 1 Hz 打印 | 保持一行一类，便于脚本解析 |
| 协议栈诊断对象 | `src/Ethercat/src/diag.c` | 若启用 `0x10F3`，需同步 ESI 对象与主站解析 |

---

## 6. 可执行 checklist

### 6.1 移植 checklist

- [ ] **P1** 新 MCU 的 SPI 主机已调通，Mode 0，8 位数据，MSB first；`BYTE_TEST` 能读回 `0x87654321`。
- [ ] **P2** 实现 `PDI_PortRead32/Read8/Write32/Write8`，并用示波器确认 CS 覆盖完整事务。
- [ ] **P3** `HW_EscRead/Write` 普通版与 ISR 版均通过读写回环（读 `0x1F00` 得 `0x5A5A5A5A`）。
- [ ] **P4** EXTI 配置正确：IRQ 低有效、下降沿触发；清序为 `INT_STS → 等 IRQ 高 → 清 pending`。
- [ ] **P5** `board_config.h` 的 SPI/CS/IRQ/SYNC0/SYNC1/RST/LED/KEY/串口与实物一致。
- [ ] **P6** `BSP_Get1msTick()` 由硬件定时器驱动，1 ms 误差 <1%；`Delay_ms()` 可用。
- [ ] **P7** 启动文件与链接脚本匹配新 MCU 的向量表、RAM/Flash 布局。
- [ ] **P8** 串口 115200 能打印启动自检与错误码。
- [ ] **P9** 主站能扫描到从站；`slaveinfo` 显示 1 个节点、SM2/SM3 长度正确。
- [ ] **P10** PREOP 下 SDO 可读 `0x1018`；SAFEOP 无 `0x001D/0x001E`；OP 下 `WKC=3`。
- [ ] **P11** 连续运行 10 min：`AL=0x08`、`stuck/dmaerr/csrTmo=0`、无中断风暴。
- [ ] **P12** 断流测试：超过看门狗窗口后 `AL=0x0014`、`CODE=0x001B`，按 `0x0014→0x0004→0x0008` 可恢复。

### 6.2 改功能 checklist

- [ ] **F1** `el9800appl.h` 的 PDO 映射表、分配对象、PDO 参数对象已更新，位/字节总数自洽。
- [ ] **F2** `APPL_GenerateMapping()` 计算出的输入/输出长度与预期一致（启动日志或调试器确认）。
- [ ] **F3** `APPL_InputMapping` / `APPL_OutputMapping` 的拷贝长度与偏移已同步，无越界。
- [ ] **F4** `APPL_Application` 的 LED/按键/模拟量映射与新 PDO 位定义一致。
- [ ] **F5** `ecat_def.h` 的 `MAX_PD_INPUT_SIZE/MAX_PD_OUTPUT_SIZE` 足够大。
- [ ] **F6** `docs/esi/E252_CH32V307.xml` 的 SM 长度、PDO 条目、FMMU、`0x1C12/0x1C13` 已更新。
- [ ] **F7** 由 ESI 重新生成 SII 并写入 EEPROM，回读校验 checksum 与 `0x1018`。
- [ ] **F8** 主站 IOmap/ENI 重新生成，过程数据偏移与长度一致。
- [ ] **F9** PREOP→SAFEOP 不报 `0x001D/0x001E/0x0017`；OP 下 PDO 双向数据正确。
- [ ] **F10** 连续 10 min 无 PDO 撕裂；看门狗窗口不小于帧周期的 5 倍。
- [ ] **F11** 若改了 TXPDO 影子结构，确认 ISR 与主循环之间没有混合快照。

---

## 7. 最小核对矩阵

| 步骤 | 检查点 | 判据 |
|---|---|---|
| 上电 | 串口启动行 | `BYTE_TEST=0x87654321`、`ID_REV` 可读 |
| PDI | `0x1F00` 回环 | 读回 `0x5A5A5A5A`、`csrTmo=0` |
| 扫描 | 主站 `slaveinfo` | 1 个从站，名称 `E252-CH32V307`，SM2/3 长度正确 |
| PREOP | SDO `0x1018` | Vendor/Product/Revision 与 ESI 一致 |
| SAFEOP | AL Status | `0x0004`，无 `0x001D/0x001E` |
| OP | 周期帧 | `AL=0x0008`、`WKC=3` |
| 看门狗 | 停流 100 ms | `0x0014/0x001B`，可按序恢复 |
| DC | SYNC0/SYNC1 | 1 ms 周期、脉宽约 100 µs、相位可配 |

---

## 8. 相关文档

- 编译、烧录与调试：[build.md](build.md)
- 接线与引脚：[hardware/wiring.md](hardware/wiring.md)
- 能力边界：[capability.md](capability.md)
- 性能与测量条件：[performance.md](performance.md)
- 主站导入步骤：[../README.md](../README.md)
