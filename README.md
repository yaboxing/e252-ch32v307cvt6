# E252 + CH32V307 EtherCAT IO 从站

基于 **SYS-E252 EtherCAT 从站控制器（ESC）** 与 **CH32V307VCT6** 的 EtherCAT 数字量 IO 从站参考实现。E252 通过 SPI PDI 连接 MCU，固件由 Beckhoff SSC 5.11 移植而来，可直接用支持 ESI 的第三方主站扫描、配置并进入 OP。

- 硬件：E252 DPORT_ECAT 模块（V2.2）+ CH32V307VCT6-R1 评估板
- 固件：SSC 5.11 + E252 SPI PDI 端口层 + EL9800 应用层
- 过程数据：RxPDO 6 B / TxPDO 6 B（8 路数字输出 + 8 路数字输入 + 模拟量/PWM 预留通道）
- 已实现：SOEM 忙等主站下 1 kHz 稳定 OP、CoE SDO、SM 看门狗、DC SYNC0/SYNC1

> 这是一个可复现的工程参考，不是经过 ETG 一致性认证的产品。能力边界与测试条件分别在 [能力清单](docs/capability.md) 与 [性能测试](docs/performance.md) 中给出。

---

## 已实现的功能

| 功能 | 状态 | 说明 |
|---|---|---|
| MRS 工程编译 / 烧录 / 调试 | ✅ | 板载 WCH-LinkE，支持 IDE 与命令行两种方式；步骤见 [docs/build.md](docs/build.md) |
| E252 PDI（SPI1 + DMA @24 MHz） | ✅ 支持 | 上电自检 `BYTE_TEST=0x87654321`；单次 AL 事件读约 16 µs |
| EtherCAT 状态机 INIT→PREOP→SAFEOP→OP | ✅ 支持 | 主站扫描、SDO、PDO 闭环、掉态恢复均通过 |
| CoE SDO / 邮箱 | ✅ 支持 | 对象字典可读；EMCY、EoE、FoE 未启用 |
| PDO 6 B / 6 B | ✅ 支持 | SM2 `0x1200`、SM3 `0x1300`，均三缓冲 |
| SM 看门狗 | ✅ 支持 | 默认 100 ms；停流后进入 `0x0014 / 0x001B`，可按状态机恢复 |
| DC / SYNC0 / SYNC1 | ✅ 支持 | 64 位系统时间；SYNC0 1 ms；SYNC1 为相对 SYNC0 的偏移 |
| 用户 LED / 按键 | ✅ | LED1=PA0、LED2=PA1、KEY=PA2；需 J3 跳线，见接线文档 |
| 串口诊断 | ✅ | USART1 115200，输出 `[ECAT]`、`[IRQ]`、`[WD]`、`[DCS]` 等运行指标 |

精确的“实测确认 / 文档声称 / 未验证 / 硬件受限”分级见 [docs/capability.md](docs/capability.md)。

---

## 硬件与接线

| 板卡 | 版本 | 原理图 |
|---|---|---|
| E252 DPORT_ECAT（E252 ESC） | V2.2（QMX102 载板） | [PDF](docs/hardware/E252_DPORT_ESC_QMX102_V2.2_20260427.pdf) |
| CH32V307VCT6-R1 评估板 | 1v1 | [PDF](docs/hardware/CH32V307VCT6-R1-1v1-SCH.pdf)（单板页） |

接线使用杜邦线，SPI1（PA5/PA6/PA7）+ 软件 CS（PB12）+ IRQ（PA3）+ SYNC0（PA4）+ SYNC1（PB11）+ RST（PB0）。完整引脚表、方向判定、自检矩阵、飞线影响与改线注意点见 **[docs/hardware/wiring.md](docs/hardware/wiring.md)**。

> 供电注意：DPORT 只接受 J4-15/16 的 **5 V**；J5-1/3 是 3.3 V，接错会损坏模块。E252 信号脚不耐 5 V。

---

## 快速开始

1. 按 [接线指导](docs/hardware/wiring.md) 连接两块板，完成断电通断自检后上电。
2. 按 [编译与烧录](docs/build.md) 构建并下载固件。
3. 串口应看到 `[E252] HW_Init=0 selftest=0 BYTE_TEST=0x87654321`，DPORT 电源灯常亮。
4. 用主站导入 ESI 并扫描，确认身份后进入 OP（见后文“用其他主站导入测试”）。

仓库目录：

```
.
├── README.md
├── src/                 # 固件源码（SSC 5.11 + E252 端口层 + 应用层）
├── master/
│   └── web-ui/          # 自研 Web 主站（Python 后端 + SSE + 浏览器前端）
├── third_party/         # SOEM、DynamicxEcatTool 等
└── docs/
    ├── build.md         # 工具安装、MRS/命令行编译、烧录、调试、排障
    ├── master-tools.md  # 两套主站的启动停止、能力对照、交叉核对与恢复
    ├── hardware/        # 原理图 PDF + 接线指导
    ├── img/             # 接线图片
    ├── esi/             # 主站导入用 ESI
    ├── thm/             # E252 与 LAN9252 技术手册
    ├── capability.md    # 能力清单（含证据等级）
    ├── performance.md   # 性能测试与复现条件
    └── competitor.md    # 与 LAN9252 / ET1100 的差异
```

---

## 主站工具

主站侧有两条可选路径：

- `third_party/DynamicxEcatTool/`：Qt6 桌面工具，基于 SOEM，适合总线浏览、DC/SYNC0 测试和 EEPROM 烧录
- `master/web-ui/`：本地 Web 调试台，Python 后端 + SSE 推送 + 浏览器前端，适合单从站联调、状态机切换、PDO 读写和帧间隔观察

两套工具都使用 [docs/esi/E252_CH32V307.xml](docs/esi/E252_CH32V307.xml) 识别从站；同一张网卡一次只能由一个主站占用。安装、启动、停止、能力对照和交叉核对方法见 [docs/master-tools.md](docs/master-tools.md)。

---

## 用其他主站导入测试

### 1. 导入 ESI

ESI 文件：[docs/esi/E252_CH32V307.xml](docs/esi/E252_CH32V307.xml)

| 项 | 值 |
|---|---|
| 设备名 | `E252-CH32V307` |
| Vendor ID | `0xE0000002` |
| Product Code | `0x26483052` |
| Revision | `0x00010211` |
| SM1（MBoxIn） | `0x1100` / 128 B |
| SM2（Outputs） | `0x1200` / 6 B，三缓冲 |
| SM3（Inputs） | `0x1300` / 6 B，三缓冲 |

ESI 中的 SM 地址与板上 SII（EEPROM）一致（SM1/SM2/SM3 = `0x1100/0x1200/0x1300`）。**不要**套用 ET1100/LAN9252 样例中的 SM 地址。

### 2. 网络与扫描

- 主站网卡直连 DPORT 的 PORT0 或 PORT1（100BASE-TX，铜缆）。
- 支持 ESI 的工具（TwinCAT、CODESYS、DynamicxEcatTool 等）：导入上述 XML，按 6 B / 6 B 过程数据配置从站，然后扫描。
- SOEM/pysoem 直接读取板上 SII，不需要导入 ESI；`slaveinfo` 应识别 1 个从站（名称 `E252-CH32V307`，PDO 6 B / 6 B），运行循环时确认 `WKC=3`。

### 3. 状态机与过程数据

建议顺序：`INIT → PREOP`（此时可读 SDO）`→ SAFEOP → OP`。

- 确认 `0x1018` 的 Vendor/Product/Revision 与上表一致。
- 确认 SM2/SM3 长度为 6 B；PDO 分配 `0x1C12/0x1C13` 与 ESI 一致。
- 字节序与位序以主站读到的数据为准：RxPDO byte0 bit0 控制 LED1，TxPDO byte0 bit0 反映 KEY 状态。

主站配置纪律（本板约束，详见 [performance.md](docs/performance.md)）：

- SM2（输出方向）必须使用三缓冲（控制字 bits1:0 = `00`）；
- **进入 OP 前就要开始周期发送过程数据**，OP 期间持续发送，不要“先进 OP、再开始发帧”；
- 停止过程数据超过约 100 ms 会触发 SM 看门狗（`AL=0x0014`、`CODE=0x001B`），恢复需按 `0x0014 → 0x0004 → 0x0008` 的次序重新请求状态。

### 4. 常见问题

| 现象 | 检查点 |
|---|---|
| 扫描不到从站 | 网线接在 PORT0/1；DPORT 5 V 与共地；网卡为有线千兆网卡而非 WiFi |
| 能扫描但进不了 OP | ESI/SII 的 SM 地址与长度；是否已开始发送过程数据；看门狗窗口 |
| 输出无效 / 输入恒 0 | PDO 映射与 6 B 长度；主站 ENI 是否用了旧 SM 地址 |
| SDO 访问异常 | 确认从站在 PREOP；部分主站读 `0x1C32/0x1C33` 存在兼容问题，可通过 ESC 寄存器 FPRD 读取 |

---

## 已知边界

- PDO 长度目前固定 6 B / 6 B；更大 PDO（12/24/32/68 B）未验证，需同时修改 OD、SII、ESI 与主站 IOmap。
- SYNC1 不是独立周期信号：`0x09A4` 定义的是 SYNC0 到 SYNC1 的间隔，SYNC1 由 SYNC0 派生。
- 本板 IO 固定 3.3 V；可变 VDDIO、光纤（SD 模式）能力受硬件冻结限制，未验证。
- SYNC 输出抖动 ≤15 ns 属手册指标，需示波器测量，暂无数据。
- 未做 ETG 一致性认证；相关表述仅代表本仓库在当前硬件上的结果。

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/build.md](docs/build.md) | MRS 与命令行两种工具链的安装、编译、烧录、调试与排障 |
| [docs/architecture.md](docs/architecture.md) | 固件分层、启动与数据流、移植到其他 MCU、变更 PDO/OD 的清单 |
| [docs/master-tools.md](docs/master-tools.md) | DynamicxEcatTool 与 Web 调试台的启动停止、能力对照、交叉核对与恢复 |
| [docs/hardware/wiring.md](docs/hardware/wiring.md) | 接线表、为什么这样接、自检、飞线与改线注意点 |
| [docs/capability.md](docs/capability.md) | 能力清单与证据等级（实测 / 文档 / 未验证 / 硬件受限） |
| [docs/performance.md](docs/performance.md) | 帧周期、PDI 延迟、看门狗、DC 的测试数据与复现条件 |
| [docs/competitor.md](docs/competitor.md) | 与 LAN9252、ET1100 的差异与待确认项 |
| [docs/esi/E252_CH32V307.xml](docs/esi/E252_CH32V307.xml) | 主站导入用 ESI |
| [docs/thm/README.md](docs/thm/README.md) | 器件手册：E252 数据手册与 LAN9252 中文版，含页码与章节索引 |

---

## License

[Apache License 2.0](LICENSE)
