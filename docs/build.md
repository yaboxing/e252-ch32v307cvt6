# 构建、烧录与调试

本目录说明 CH32V307VCT6 + E252 从站固件的两种构建方式：MounRiver Studio（下称 MRS）IDE，以及命令行 Makefile。两种方式使用同一套源文件、宏、头文件路径、链接脚本和输出目录，产物应当一致。

## 1. 目录与产物

固件工程根目录是仓库内的 `src/`，打开、编译、烧录都以它为根。

```
src/
├── .project / .cproject        MRS 工程配置
├── .template                   MRS 下载配置（芯片型号、地址、烧录动作）
├── .mrs/                       MRS 工作区描述
├── Makefile                    命令行构建
├── app/                       应用层（main、EL9800 应用）
├── bsp/                        板级配置（引脚、时钟、指示灯）
├── hal/                        E252/LAN9252 PDI 与 SPI 驱动
├── Ethercat/                   SSC 从站栈与平台适配层
├── sdk/                        WCH 外设库、启动文件、链接脚本
└── out/                        构建输出（不入库）
```

构建产物：

| 文件 | 说明 |
| --- | --- |
| `out/e252-ch32v307cvt6.elf` | 带调试信息，用于调试与反汇编 |
| `out/e252-ch32v307cvt6.hex` | Intel HEX，MRS 下载和 OpenOCD 烧录使用 |
| `out/e252-ch32v307cvt6.bin` | 纯二进制镜像 |
| `out/e252-ch32v307cvt6.map` | 链接映射表 |

## 2. 工具安装

### 2.1 MRS（Windows）

安装 MounRiver Studio 3，安装时勾选 RISC-V 工具链（WCH 提供的 RISC-V Embedded GCC15）。默认安装后包含：

- 编译器：`riscv32-wch-elf-gcc`、`riscv32-wch-elf-objcopy`、`riscv32-wch-elf-size`
- GNU make：MRS 自带，用于 IDE 内部构建
- OpenOCD：WCH 定制版，配合 `wch-riscv.cfg`
- WCH-Link 驱动：板载 WCH-LinkE 通过 USB 枚举后可直接使用

MRS 默认安装在 `C:\MounRiver\MounRiver_Studio3`，下文以该目录为例；安装到其它位置时按实际路径替换。工具链与 OpenOCD 位于：

```
C:\MounRiver\MounRiver_Studio3\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC15\bin
C:\MounRiver\MounRiver_Studio3\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin
```

### 2.2 命令行（Windows）

把工具链 bin 目录加入当前终端 `PATH`，或者每次构建时用 `TOOLCHAIN_BIN` 传入（推荐，避免污染全局环境）：

```bat
set TOOLCHAIN_BIN=C:\MounRiver\MounRiver_Studio3\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC15\bin
set PATH=%TOOLCHAIN_BIN%;%PATH%
```

make 可直接使用 MRS 自带的 `make.exe`，也可以使用其它 GNU make（需支持 `$(eval)` 与 `$(foreach)`）。

### 2.3 命令行（Linux / WSL，可选）

固件依赖 WCH 工具链的启动文件、链接脚本与库。若要在 Linux 侧构建，需要 `riscv32-wch-elf-` 前缀的交叉工具链，或把 `CROSS` 覆盖为本地可用的前缀：

```sh
make CROSS=riscv-none-elf-
```

使用其它前缀时请自行确认 `-march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8`、`-nostartfiles`、`-specs=nosys.specs` 与 `sdk/Ld/Link.ld` 均被工具链支持。烧录侧使用任意可用的 OpenOCD 与 WCH RISC-V 配置文件。

## 3. 方式一：MounRiver Studio

1. 打开工程：MRS 中选择打开文件夹，指向仓库的 `src/`。工程名与配置取自 `.project` / `.cproject`，构建配置名为 `obj`。
2. 检查工具链：若 IDE 提示找不到 `riscv32-wch-elf-gcc`，在 MRS 的工具链设置里选择已安装的 RISC-V Embedded GCC15。
3. 编译：执行 Build（增量）或 Rebuild（全量）。输出到 `src/out/`，文件名见上表。
4. 烧录：板载 WCH-LinkE 已枚举、目标板已供电时，直接点下载。下载配置在 `.template` 中：

   - MCU：`CH32V307VCT6`
   - 下载地址：`0x08000000`
   - 目标文件：`out/e252-ch32v307cvt6.hex`
   - 动作：全片擦除、编程、校验、复位运行
   - 调试器：WCH-Link

5. 调试：新建/使用 MRS 的调试配置，调试器选 WCH-Link，接口为 SDI（板载直连），芯片选 CH32V307VCT6。可下断点、单步、查看外设寄存器。
6. 串口：调试串口为 USART1（PA9/PA10，115200 8N1），固件通过 `printf` 输出运行状态（链路、AL 状态、事件计数、DC/SYNC 统计等）。

## 4. 方式二：命令行

所有命令都在 `src/` 下执行。

```sh
# 查看可用目标
make help

# 构建（默认目标）：elf + hex + bin + map
make

# 指定工具链目录
make TOOLCHAIN_BIN="C:/MounRiver/MounRiver_Studio3/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC15/bin"

# 打印段大小
make size

# 清理（删除整个 out/）
make clean

# 烧录（构建后通过 OpenOCD + WCH-LinkE 编程并校验）
make flash OPENOCD="C:/MounRiver/MounRiver_Studio3/resources/app/resources/win32/components/WCH/OpenOCD/OpenOCD/bin/openocd.exe" \
           OPENOCD_CFG="C:/MounRiver/MounRiver_Studio3/resources/app/resources/win32/components/WCH/OpenOCD/OpenOCD/bin/wch-riscv.cfg"
```

可用变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `CROSS` | `riscv32-wch-elf-` | 工具链前缀 |
| `TOOLCHAIN_BIN` | 空（用 PATH） | 工具链 bin 目录 |
| `OPT` | `-Os` | 优化等级 |
| `DEBUG` | `-g -gdwarf-4` | 调试信息；置空可得到更小的 ELF |
| `CSTD` | 空（工具链默认） | 语言标准，一般不需要指定 |
| `OPENOCD` | `openocd` | OpenOCD 可执行文件 |
| `OPENOCD_CFG` | `wch-riscv.cfg` | WCH RISC-V 配置文件 |
| `SPEED` | `6000` | 调试时钟 kHz |

Makefile 与 MRS 工程保持一致的关键点：

- 源文件集合：`app`、`bsp`、`Ethercat/src`、`Ethercat/port/el9800hw.c`、`hal`、`sdk/Core`、`sdk/Peripheral/src`，外加启动文件 `sdk/Startup/startup_ch32v30x_D8C.S`；链接时启动文件在最后。
- 编译参数：`-march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -Os -ffunction-sections -fdata-sections -DUSE_STDPERIPH_DRIVER`。
- 头文件路径：项目根、`Ethercat/Inc`、`Ethercat/port`、`app`、`hal`、`bsp`、`sdk/Core`、`sdk/Peripheral/inc`。
- 链接：`-T sdk/Ld/Link.ld -nostartfiles -specs=nosys.specs -Wl,--gc-sections`。
- 每个目录内按文件名排序，目录顺序固定。RISC-V 链接期 relaxation 对符号顺序敏感，改动链接顺序会改变最终代码尺寸。

## 5. 一致性验证

使用 MRS 工具链实测：命令行构建产生 53 个目标文件、2 条警告（`Ethercat/src/objdef.c` 的 `-Wpointer-to-int-cast`），段大小为 `text=81268`、`data=3324`、`bss=5104`。`out/e252-ch32v307cvt6.bin`（84592 字节）与 `out/e252-ch32v307cvt6.hex`（237971 字节）与既有构建脚本的产物逐字节相同（SHA256 一致）。

ELF 默认带 `-g -gdwarf-4`，因此比不带调试信息的 ELF 大；`-g` 只影响调试段，不影响 `.hex`/`.bin` 的机器码。

MRS 工程侧已逐项对齐上述宏、包含路径、链接脚本与源集合，并以 `.cproject`/`.project`/`.template`/`.mrs` 固化；图形界面中的打开与调试尚未实测，已核对工程结构与选项取值。

## 6. 常见问题排查

**找不到编译器：`'riscv32-wch-elf-gcc' 不是内部或外部命令`**
工具链不在 PATH。设置 `TOOLCHAIN_BIN` 或在 MRS 中重新选择工具链。

**`make` 报 `sh: syntax error: unexpected end of file (expecting "then")`**
Makefile 的 recipe 必须是 POSIX shell 语法。MRS 自带的 make 会调用同目录的 `sh`；不要写成 `if not exist ...`、`rmdir /S` 这类 cmd 语法。本仓库的 Makefile 已统一使用 `mkdir -p`、`rm -rf`。

**在 `\\wsl.localhost\...` 这类 UNC 路径下执行 make 失败**
Windows 的 `cmd.exe`/部分工具不接受 UNC 作为当前目录。把仓库放到本地盘（如 `C:\work\...`），或在 WSL 内部用 Linux 工具链构建。

**路径包含空格或中文**
命令行里给变量和路径加双引号。MRS 安装在 `Program Files` 下时尤其注意。

**OpenOCD 找不到下载器**
先确认 WCH-LinkE 已枚举（USB 设备为 `VID_1A86&PID_8010`），换数据线/换 USB 口，必要时重装 WCH-Link 驱动。`make flash` 卡在 `init` 通常是下载器或目标板供电问题。

**OpenOCD 报找不到固件文件**
`make flash` 依赖 `out/*.hex`。先执行 `make`，或确认没有先跑 `make clean`。

**OpenOCD 下载路径中的反斜杠丢失**
OpenOCD 的 Tcl 解析会吞掉 `\`。本仓库的 `flash` 目标使用 make 的 `abspath` 生成路径；手工调用 OpenOCD 时请把路径写成 `C:/dir/fw.hex`。

**编译通过但 IDE 标红/找不到头文件**
确认打开的是 `src/` 目录（而不是仓库根），并确认 `.cproject` 的 include 路径仍为项目相对路径。

**链接时报未定义符号或段溢出**
确认 `sdk/Ld/Link.ld` 与 `sdk/Startup/startup_ch32v30x_D8C.S` 存在，且没有误删 `sdk/`、`Ethercat/port/`。`-nostartfiles` 下启动文件必须显式参与链接。

**串口没有输出**
USART1 使用 PA9/PA10，115200 8N1；确认 USB 串口接到了这两个脚，且没有把 PA9/PA10 复用为其它功能。

**烧录后程序不运行**
检查 `.template` 中的下载地址是否为 `0x08000000`，以及是否勾选了复位运行。若之前改过主频/时钟配置，确认 HSE 晶振已焊接。

## 7. 未验证项

- 烧录与调试流程未在实机执行（未占用板卡、串口与网卡）；`make flash` 的 OpenOCD 命令构造已通过 `make -n` 检查，实机烧录需在有板卡的环境验证。
- MRS 图形界面中的打开、编译、单步调试未实测；工程文件按 MRS 工程格式生成并与命令行参数对齐。
