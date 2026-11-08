# 主站工具：DynamicxEcatTool 与 Web 调试台

本仓库提供两条主站路径：`third_party/DynamicxEcatTool/` 的桌面工具，以及 `master/web-ui/` 的本地 Web 调试台。两者都通过 `docs/esi/E252_CH32V307.xml` 识别从站，都直接占用一张有线网卡。同一张网卡在同一时刻只能由一个主站打开，切换前必须彻底退出前一个实例。

## 1. 选型速览

| 维度 | DynamicxEcatTool | Web 调试台 |
|---|---|---|
| 形态 | Qt6 + QML 桌面程序，C++ 调用 SOEM | Python 本地 HTTP 服务 + SSE 推送 + 浏览器前端 |
| 位置 | `third_party/DynamicxEcatTool/`（Git 子模块） | `master/web-ui/` |
| 运行平台 | Windows 11、Ubuntu 20.04 / 24.04 | Windows；Linux 需 libpcap 与抓包权限 |
| 访问方式 | 本机 GUI，无 TCP 端口 | 默认只监听 `127.0.0.1:8765` |
| 强项 | DC SYNC0 配置与回读、EEPROM 烧录、总线浏览器、SDO/PDO 浏览 | 状态机逐步切换与 ACK、实时 PDO/EMCY/AL 事件流、帧间隔与邮箱耗时观测 |
| 不适合 | 无人值守自动测试、跨机远程操作 | 产品级控制、DC 配置、EEPROM 烧录 |

单从站联调优先用 Web 调试台：页面刷新快，状态码、PDO 字节和帧间隔都在同一屏。需要改 SII/EEPROM 或核对 DC/SYNC0 时用 DynamicxEcatTool。

## 2. DynamicxEcatTool

### 2.1 依赖

Windows：

- Npcap，安装时选择 WinPcap API-compatible Mode
- 管理员权限运行；普通用户可能枚举不到网卡或打开失败
- 预编译安装包已包含 Qt、SOEM、yaml-cpp、VC++ 运行库和 Npcap 安装器，无需另行安装开发库

Ubuntu：

- AppImage 或 deb 安装包，按系统版本选择 20.04 / 24.04 产物
- EtherCAT 通信需要 root 或 raw socket 权限
- 图形界面需要 X11 或 XWayland；建议安装中文字体

从源码构建需要 CMake 3.28 及以上、支持 C++17 的编译器、Qt6（Core / Quick / QuickControls2）、yaml-cpp，以及仓库锁定的 SOEM 子模块。构建和打包步骤见 [DynamicxEcatTool 说明](../third_party/DynamicxEcatTool/README.md) 与 [打包说明](../third_party/DynamicxEcatTool/packaging/README.md)。

### 2.2 安装与启动

Windows 运行安装器，安装完成后以管理员身份启动 `DynamicxEcatTool.exe`。Ubuntu deb 安装后可用 `dynamicx-ecat-tool` 启动；AppImage 先加执行权限再运行，缺少 FUSE 时用 `--appimage-extract` 解包后运行 `AppRun`。

启动后在网卡下拉框刷新并选择连接 E252 的那张网卡，再连接总线。程序没有监听端口，确认是否在运行看主窗口和进程，不看端口。

### 2.3 启动成功的判据

- 主窗口正常显示，网卡下拉框能列出至少一张有线网卡
- 点击连接后日志出现从站数量和扫描结果
- 总线浏览器能读到从站身份、SM、PDO 映射与对象字典

### 2.4 用它做什么

- **总线浏览器**：扫描总线，逐级切换 INIT / PREOP / SAFEOP / OP，按 ESI 名称读写 PDO 变量，读写 SDO，浏览在线对象字典。入口在 [EthercatExplorerController.h](../third_party/DynamicxEcatTool/Backend/Explorer/EthercatExplorerController.h)。
- **DC 测试**：在 PREOP 配置 SYNC0，回读激活位与周期，再尝试进入 OP；页面显示参考时钟、DC 时间、WKC、周期偏差与超期统计，以及各从站的 DC/SYNC0 状态。周期范围 250 µs 到 1 s，默认 2 ms、偏移 0。实现见 [DcTestController.cpp](../third_party/DynamicxEcatTool/Backend/Dc/DcTestController.cpp)。
- **EEPROM 烧录**：选择从站与 `.hex` 文件写入 EEPROM，用于更新 SII。改 PDO 长度或 SM 配置后，SII 必须同步重烧。

DC 页面显示的是主机侧相位和周期统计，不是从站 SYNC0 引脚上的抖动；引脚时序仍需示波器测量。测试界面里的电机参数页与 E252 无关，联调 E252 时不使用。

## 3. Web 调试台

### 3.1 组成

`master/web-ui/` 是一个单进程应用：

- [main.py](../master/web-ui/main.py)：参数解析、启动 HTTP 服务、自动打开浏览器
- [backend/ecat.py](../master/web-ui/backend/ecat.py)：pysoem 主站、周期线程、邮箱/EMCY 线程、状态机、PDO 与 SDO 操作
- [backend/server.py](../master/web-ui/backend/server.py)：REST 接口与 SSE 事件流，同时提供静态页面
- `web/`：`index.html`、`app.js`、`style.css`，无构建步骤

浏览器只是客户端，后端和前端在同一个 Python 进程里，不需要单独启动前端服务。

### 3.2 依赖与安装

- Python 3.10 及以上
- `pysoem` 1.1.13 及以上，由 `requirements.txt` 声明
- Windows 需 Npcap（WinPcap API-compatible Mode），建议管理员权限
- Linux 需 libpcap，并以 root 或具备 `CAP_NET_RAW` 的权限运行

在仓库根目录执行：

```bash
cd master/web-ui
python -m pip install -r requirements.txt
```

### 3.3 启动

```bash
python main.py --esi ../../docs/esi/E252_CH32V307.xml
```

默认监听 `127.0.0.1:8765`，启动后终端打印界面地址，浏览器访问 `http://127.0.0.1:8765/`。

常用参数：

| 参数 | 作用 |
|---|---|
| `--port 8790` | 换监听端口，默认 8765 |
| `--host 0.0.0.0` | 改监听地址；默认只允许本机访问 |
| `--no-browser` | 不自动打开浏览器 |
| `--esi ../../docs/esi/E252_CH32V307.xml` | 本仓库没有 `firmware/` 目录，必须显式指定；否则 PDO 与对象名称退回原始索引 |

`run.ps1` 会先检查 `pysoem`，缺失时自动安装，再启动服务，支持 `-Port` 与 `-NoBrowser`。它不传 `--esi`，默认路径不存在，PDO 与对象名称会退回原始索引，建议仍用上面的显式命令。

### 3.4 启动成功的判据

- 终端打印界面地址、ESI 路径和当前权限
- `http://127.0.0.1:8765/` 能打开，页面顶部网卡下拉框有内容
- 选择网卡并点击连接后，日志出现 `config_init` 的从站数量、`config_map` 的 IO 字节数和 WKC
- 从站列表出现条目，状态进入 INIT 或更高

### 3.5 能做什么

- **状态机**：INIT / PREOP / SAFEOP / OP 逐步切换，失败时返回当前最低状态和每个从站的 AL 状态码；单独的 ACK 按钮用于清除 AL 错误后重试。
- **PDO**：实时显示输入、输出字节；按位偏移和位长度写输出字段，也可整帧写原始字节；另有 6 字节 IO 演示命令。写入只修改主站输出缓冲，只有 OP 下周期帧才会真正下发。
- **SDO 与对象字典**：任意索引/子索引读写，浏览对象字典、PDO 映射和对象值；连接时还会比对 ESI 与在线 0x1018 身份。
- **邮箱与 EMCY**：独立线程每 200 ms 轮询 `mbx_receive`，EMCY 会解码常见错误码；邮箱轮询耗时单独计时。
- **过程数据质量**：周期默认 2 ms，可在 0.5–50 ms 调节；显示 WKC、平滑帧率、抖动、当前与最大帧间隔、超过 50 ms 的停顿次数、停顿后的补发帧数、邮箱轮询耗时。
- **诊断事件**：AL 状态历史、状态变化事件、TxPDO byte0 bit0 的按键边沿计数。
- **事件推送**：PDO 或状态变化通过 SSE 立即推送，另有 1 s 快照兜底；页面关闭后重开可继续接收。

### 3.6 不能做什么

- 不能配置或关闭 DC/SYNC0，没有 `config_dc` 调用，也没有读取 DC 寄存器的页面；DC 相关核对只能在 DynamicxEcatTool 侧做。
- 不能烧录 EEPROM/SII。
- 不读取 ENI，不做主站配置工程，不做冗余、热插拔或分布式时钟规划。
- 不是产品级主站：单进程、单网卡、单用户、无鉴权、无持久化日志，Python 线程调度不适合硬实时。
- 页面只监听本机，默认不对外网开放；`--host 0.0.0.0` 只建议在隔离网络短时使用。

## 4. 测试能力对照

| 能力 | DynamicxEcatTool | Web 调试台 | 说明 |
|---|---|---|---|
| 网卡扫描与连接 | 支持 | 支持 | 都通过 SOEM/pysoem 打开同一张网卡 |
| 状态机 INIT/PREOP/SAFEOP/OP | 总线浏览器逐级切换 | 逐级切换，带 ACK | 失败时都显示最低状态与 AL 码 |
| 状态恢复 | 重新扫描并切状态；DC 测试可重新进入 OP | ACK 后重切；断开重连会重新初始化 | 从站被看门狗打回 SAFEOP 时先 ACK |
| PDO 输入读取 | 总线浏览器按变量显示 | 实时字节、按键事件 | 两边读的是同一份 TxPDO |
| PDO 输出写入 | 总线浏览器按变量写 | 按位/整帧写，另有演示命令 | 都在 OP 下才真正下发 |
| SDO 读写 | 支持 | 支持 | Web 还提供对象字典浏览与身份比对 |
| 看门狗 | 无专用界面；可用 SDO 写 0x0400 / 0x0420 | 无专用界面；可用 SDO 写 | 两边都能观察 AL 0x0014 / 代码 0x001B |
| 看门狗归因 | 日志与 AL 状态 | 帧间隔、停顿次数、补发帧数、邮箱耗时 | 判断 0x001B 是否由主站停顿引起时 Web 更直接 |
| DC/SYNC0 配置 | 支持，250 µs–1 s | 不支持 | 只有 DynamicxEcatTool 能打开 SYNC0 |
| DC 观测 | 参考时钟、DC 时间、周期偏差、超期、从站状态 | 无专用页面 | 两边都不测 SYNC0 引脚抖动 |
| 邮箱/EMCY | SDO 走邮箱，日志显示错误 | 轮询 `mbx_receive`，EMCY 解码，AL 历史 | 高频邮箱压力测试用 Web 的耗时指标 |
| EEPROM/SII 烧录 | 支持 `.hex` | 不支持 | 改 SII 后用 DynamicxEcatTool |
| 帧周期设置 | DC 测试页 250 µs–1 s | 0.5–50 ms | 普通过程数据周期用 Web |
| 帧间隔/抖动观测 | DC 页显示主机周期与超期 | 显示平滑帧率、抖动、最大间隔、补发 | 口径不同，不直接比较绝对数值 |
| 多从站 | 支持 | 支持，界面以单从站操作为主 | 本仓库以单个 E252 为对象 |
| 远程访问 | 无 | 仅本机回环 | 两者都不作为远程主站使用 |

## 5. 用两套主站交叉核对

### 5.1 准备

1. 使用同一份 ESI：`docs/esi/E252_CH32V307.xml`。DynamicxEcatTool 在总线浏览器里把 ESI 目录指向 `docs/esi/`；Web 启动时加 `--esi ../../docs/esi/E252_CH32V307.xml`。
2. 保持接线、从站地址、网卡和从站位置不变。只测一个从站，避免多从站互相影响。
3. 每次只启动一套主站。切换前按第 6 节的停止步骤确认旧进程、端口和网卡都已释放。
4. 记录测试条件：周期、运行时长、是否启用 DC、主站所在操作系统。两套主站的调度实现不同，绝对延迟不可直接对比。

### 5.2 对比步骤

1. **身份与配置一致性**：在两套工具里分别读 Vendor ID、Product Code、Revision、SM 起止地址与长度、0x1C12/0x1C13 的 PDO 分配。不一致时先怀疑 ESI 版本或 SII 未更新，再怀疑接线。
2. **状态机**：都从 INIT 开始，按 PREOP → SAFEOP → OP 逐级切换，记录每级耗时和 AL 状态码；再人为让从站报错（例如改错 SM 长度），比较两边的失败提示与恢复路径。
3. **PDO 一致性**：用同一个输出字节序列，例如 byte0 依次写 0x01、0x02、0x00，观察物理 LED 和两套工具读回的 TxPDO。Web 可按位写，DynamicxEcatTool 按变量写，写入后都应在下一周期生效。
4. **看门狗**：通过 SDO 写入相同的 0x0400 / 0x0420，进入 OP 后停止周期帧，记录 AL 变为 0x0014、代码 0x001B 的时间。Web 同时看帧间隔和补发计数，排除主站自身停顿；DynamicxEcatTool 看 AL 状态与日志。
5. **帧周期**：两边都设 2 ms，连续运行相同时间，比较 WKC、从站状态是否稳定、是否出现 0x001B。绝对帧率只在同一台机器、同一张网卡、同一周期下横向参考。
6. **DC**：只在 DynamicxEcatTool 侧配置 SYNC0，记录回读的激活位、周期和从站 DC 状态；用示波器测 SYNC0 引脚。Web 不参与 DC 配置，DC 结束后先关闭 SYNC0 并重新上电，再交给 Web 做普通过程数据测试。

### 5.3 只能在一侧观测的指标

- 仅 DynamicxEcatTool：DC 参考时钟、DC 时间、SYNC0 配置回读、从站 DC/SYNC0 状态、EEPROM 烧录结果、主机相位与超期统计。
- 仅 Web 调试台：邮箱单次轮询耗时、帧间隔最大值、超过 50 ms 的停顿次数、停顿补发计数、平滑帧率、EMCY 解码文本、AL 状态历史。
- 仅外部仪器：SYNC0/SYNC1 引脚的实际抖动与相位、SPI 波形、电源纹波。

### 5.4 结论怎么下

- 两套工具读到相同的身份、SM、PDO 分配和状态迁移结果，说明 ESI/SII 与从站实现一致，可排除主站侧解析问题。
- 只有一套能进 OP，先检查该主站的 ESI 路径、周期设置、DC 状态和网卡独占情况，再检查接线。
- 两套都能进 OP 但 PDO 字节不同，优先核对 PDO 映射、字节序和输出缓冲长度，不要先怀疑硬件。
- 帧率或延迟差异属于主站实现差异；从站侧能力以两套工具结果一致、且可测量的数值为准。

## 6. 启动、停止与重启

### 6.1 DynamicxEcatTool

**启动**

1. 确认没有其他主站占用网卡；Windows 以管理员身份运行，Ubuntu 用 root 或授予 raw socket 权限。
2. Windows 启动 `DynamicxEcatTool.exe`；Ubuntu deb 用 `dynamicx-ecat-tool`，AppImage 直接运行。
3. 刷新网卡列表，选择连接 E252 的网卡，连接总线。
4. 看到从站数量、日志和总线浏览器内容，说明主站已在跑。程序没有监听端口，不需要检查端口。

**停止**

关闭主窗口即退出进程；程序没有托盘常驻或后台服务。退出时 `EthercatMasterController` 的析构会调用 `stop()`，释放主站和网卡。

确认进程退出：

```powershell
Get-Process DynamicxEcatTool -ErrorAction SilentlyContinue
```

Linux：

```bash
pgrep -af DynamicxEcatTool
```

两条命令都应无输出。窗口已关闭但进程仍在时，用任务管理器或 `Stop-Process` / `kill` 结束。

**重启**

重新启动程序，刷新网卡并连接即可。从站可能仍停在 OP 或已被看门狗打回 SAFEOP；重新扫描会重新初始化总线，若 AL 有错误先在总线浏览器里 ACK，再逐级切到 OP。仍然进不了 OP 时给从站断电重上电。

### 6.2 Web 调试台

**启动**

1. 确认没有其他主站占用网卡，且 8765 端口空闲。
2. 在 `master/web-ui/` 下执行安装命令（只需一次），再执行 `python main.py --esi ../../docs/esi/E252_CH32V307.xml`。
3. 浏览器打开 `http://127.0.0.1:8765/`，选择网卡并连接。
4. 页面出现从站、WKC 和 IO 字节数，说明后端与总线都在工作。

**停止**

在启动终端按 `Ctrl+C`。`main.py` 的 `finally` 会调用 `service.disconnect()` 和 `httpd.server_close()`，先断开从站再释放端口。浏览器标签页可以留着，后端已停，页面不会再更新；关闭标签页不是停止服务的必要条件。

终端丢失时按端口找进程：

```powershell
Get-NetTCPConnection -State Listen | Where-Object LocalPort -eq 8765 |
  ForEach-Object { Stop-Process -Id $_.OwningProcess }
```

确认退出：

```powershell
Get-NetTCPConnection -State Listen | Where-Object LocalPort -eq 8765
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'master\\web-ui|main\.py' }
```

两条命令都应无输出。Linux：

```bash
ss -ltnp | grep 8765
pgrep -af 'master/web-ui|main.py'
```

**重启**

端口和进程确认释放后，重新执行启动命令即可，不需要重装依赖。后端代码改动必须重启进程；`web/` 下的静态文件改完刷新浏览器即可。从站侧处理同 DynamicxEcatTool：重新连接并扫描，AL 报错先 ACK，必要时断电重上电。

### 6.3 两套工具的差异

| 项目 | DynamicxEcatTool | Web 调试台 |
|---|---|---|
| 进程模型 | 单 GUI 进程 | 单 Python 进程，浏览器是独立客户端 |
| 关闭窗口/标签 | 关主窗口即退出 | 关标签页不停止后端；必须停终端或结束进程 |
| 后台残留 | 无托盘、无服务 | 终端关闭后进程通常随之退出；异常时按端口查 PID |
| 端口 | 无 | 默认 8765，可改 |
| 网卡释放 | 进程退出时释放 | `Ctrl+C` 走 disconnect 后释放；强杀后由操作系统回收 |
| 额外关闭动作 | 无 | 不需要关浏览器；需要停后端 |

### 6.4 常见卡住的情况

**端口被占用**：启动时提示无法监听 8765。换端口 `--port 8790`，或按上面的端口反查命令结束旧进程。不要两个实例共用同一端口。

**网卡仍被上一个实例持有**：表现为 `open` 失败、`config_init` 返回 0、页面一直扫不到从站。先确认没有残留的 `python main.py`、`DynamicxEcatTool` 或 `slaveinfo` 进程；结束进程后等几秒再连接。仍然失败时重启 Npcap 服务或重启系统，排除驱动仍持有句柄。

**进程假死**：Web 页面不更新、`Ctrl+C` 无响应时，按端口找 PID 后结束；若结束失败，用任务管理器强制结束。DynamicxEcatTool 窗口无响应时同样强制结束。进程真正退出后网卡才会释放，端口才会释放；不要在半退出状态下启动另一套主站。

**从站不响应**：先看 AL 状态码。0x001B 通常是主站停止发帧触发 SM 看门狗；恢复后 ACK 再切状态。若主站和从站都正常但扫描不到，检查网线接在 PORT0/1、DPORT 供电和共地。

## 7. 网卡独占与从站恢复

### 7.1 为什么必须彻底退出

SOEM 和 pysoem 都以独占方式打开网卡。两个主站同时运行时，后启动的一个通常打不开网卡，或能打开但收不到帧、WKC 为 0；两边还会互相抢帧，导致状态机和看门狗判断失真。切换主站前必须让前一个进程真正退出，而不是只关窗口或只关浏览器标签。

### 7.2 切换前的检查

1. 停止周期发送并断开从站（Web 用断开按钮或 `Ctrl+C`；DynamicxEcatTool 关闭窗口）。
2. 确认进程、端口、网卡都已释放，检查命令见第 6 节。
3. 等待至少一个看门狗周期（100 ms），让从站进入安全状态，再启动另一套主站。
4. 如果刚跑过 DC 测试，先关闭 SYNC0 并给从站断电重上电，再启动 Web 调试台。

### 7.3 异常退出后的恢复

主站异常退出后，E252 收不到过程数据，SM 看门狗到期会把 AL 置为 0x0014、代码 0x001B，输出进入安全状态。恢复步骤：

1. 结束残留进程，确认网卡和端口释放。
2. 重新启动主站并扫描；新主站会重新初始化总线。
3. 若 AL 仍为 0x0014，先 ACK，再 INIT → PREOP → SAFEOP → OP。
4. 若 DC 曾被打开且无法关闭，或状态机反复失败，给从站断电重上电，回到干净状态。

正常退出不需要给从站重新上电；只有 DC 配置残留、AL 无法 ACK 或总线状态异常时才需要断电。

## 8. 定位与边界

Web 调试台是联调用的观察工具，不是产品级主站。它适合单从站、短时间、有人值守的调试：看状态码、改 PDO、抓 EMCY、判断主站停顿。它不具备 ENI 工程、冗余、热插拔、分布式时钟规划、持久化记录、权限控制和生产级实时性，不应接入运动控制或安全相关回路。

DynamicxEcatTool 是功能更完整的工程调试工具，能改 EEPROM、做 DC SYNC0 测试和总线浏览，但同样不是产品级主站，不承担产线控制、冗余切换或长期无人值守运行。

两套工具测出的数据只代表当前硬件、当前接线和当前主站配置；涉及 SYNC 引脚抖动、SPI 时序和电源质量时，以示波器测量为准。

## 9. 相关文档

- [接线指导](hardware/wiring.md)：网卡、DPORT 与 CH32V307 的接线和自检
- [ESI](esi/E252_CH32V307.xml)：两套主站共用的设备描述
- [能力清单](capability.md)：从站侧能力与证据等级
- [性能测试](performance.md)：帧周期、PDI 延迟、看门狗与 DC 的测试条件
- [固件架构](architecture.md)：PDI、SM、PDO 映射与看门狗在固件中的位置
- [编译与烧录](build.md)：固件工具链与下载
