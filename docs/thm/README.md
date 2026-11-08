# 技术手册

本目录保存两颗器件原厂手册的只读副本，用于查寄存器地址、SYNC/DC 语义、PDI 命令与时序。固件当前的实现与能力边界不在这里，见 [capability.md](../capability.md)、[performance.md](../performance.md) 与 [architecture.md](../architecture.md)。

| 文件 | 器件 | 文档 | 页数 |
|---|---|---|---|
| [E252_EtherCAT_Controller_Manual.pdf](E252_EtherCAT_Controller_Manual.pdf) | SYS-E252（DPORT_ECAT V2.2 模块上的 EtherCAT 从站控制器） | SYS-E252 数据手册 V1.1，2026-03 | 184 |
| [LAN9252_Technical_Manual_CN.pdf](LAN9252_Technical_Manual_CN.pdf) | LAN9252（Microchip EtherCAT 从站控制器，2/3 端口） | DS00001909A_CN 中文版，2015 | 329 |

## E252_EtherCAT_Controller_Manual.pdf

- 器件：SYS-E252，64 引脚 QFN/TQFP-EP，8 FMMU、8 SyncManager、8 KB DPRAM，SPI/QSPI PDI。
- 版本：V1.1，2026-03。文首有 V1.0/V1.1 的版本修正记录。
- 查什么：
  - 寄存器映射与系统控制/状态寄存器：§6，印刷页 16 起；
  - 引脚分配与电源连接：§4–§5，印刷页 3 起；
  - SPI/SQI 从接口、读/写命令、SPI 时序：§11，印刷页 59 起（时序要求 §11.3，页 75）；
  - 分布式时钟、SYNC/LATCH 引脚复用与脉冲长度：§13.2，印刷页 89 起；
  - SyncManager 寄存器：§13.14.60，印刷页 151 起；
  - EEPROM/I²C 接口：§14，印刷页 168 起；
  - 封装与选型：§18–§19，印刷页 175 起。
- 页码换算：PDF 物理页 = 印刷页码 + 8（封面、版本记录、目录占前 8 页）。
- 版权：手册内没有版权或许可声明；末尾“联系方式”页含厂商地址、个人姓名与电话。对外再分发前需向厂商确认授权。

## LAN9252_Technical_Manual_CN.pdf

- 器件：LAN9252，64 引脚 QFN/TQFP-EP，3 FMMU、4 SyncManager、4 KB DPRAM，8/16 位主机总线或 SPI/SQI。
- 文档编号：DS00001909A_CN（中文版），© 2015 Microchip Technology Inc.。
- 查什么：
  - 寄存器映射：§5.0，页 32 起；
  - SPI/SQI 从接口、命令与时序：§10.0，页 102 起（时序要求 §10.3，页 119）；
  - 分布式时钟与 SYNC/LATCH 时序：§12.2，页 197 起；
  - EtherCAT CSR 与过程数据 RAM 的间接访问流程：§12.11，页 208 起；
  - SyncManager 寄存器：§12.14.60，页 272 起；
  - EEPROM/I²C 接口：§13.0，页 295 起；
  - 电气特性、时序与封装：§18–§19，页 307 起。
- 页码换算：PDF 物理页与页脚印刷页码一致。
- 版权：Microchip 数据手册，引用和再分发遵循 Microchip 的文档使用条款；仓库内保留原始文献编号。

## 使用提示

- 两份手册描述的是芯片能力，不等于本板固件已实现的能力；实际可用项以能力清单的等级标注为准。
- 涉及 SYNC 引脚、SPI 时钟上限和看门狗窗口时，先按手册确认寄存器语义，再对照本仓库的 ESI、SII 与端口层实现。
- 手册只读；需要标注或摘录时，在文档里写章节号与页码，不要改动 PDF 本体。
