# E252 EtherCAT 主站调试台 · 使用指南

本机 Web 主站调试台（pysoem + ThreadingHTTPServer + 无构建前端），默认监听 `127.0.0.1:8765`。

## 一、从零启动（照抄即可）

在仓库根目录执行：

```powershell
# 1) 进入工具目录
cd master\web-ui

# 2) 首次使用：安装依赖 pysoem（已装可跳过）
python -m pip install -r requirements.txt

# 3) 启动（自动打开浏览器）
python main.py
```

浏览器地址（启动后终端会打印同样的地址）：

```text
http://127.0.0.1:8765/
```

一条命令的等价写法（`run.ps1` 会自动检查并安装 pysoem，再以 8765 端口启动）：

```powershell
cd master\web-ui
.\run.ps1
```

## 二、常用参数

| 需求                    | 命令                                                      |
| ----------------------- | --------------------------------------------------------- |
| 换端口（8765 被占用时） | `python main.py --port 8790`                            |
| 不自动打开浏览器        | `python main.py --no-browser`                           |
| 指定 ESI XML            | `python main.py --esi ..\..\docs\esi\E252_CH32V307.xml` |
| 只监听本机（默认）      | `python main.py --host 127.0.0.1`                       |

ESI 默认取仓库内 `docs/esi/E252_CH32V307.xml`（相对本目录为 `..\..\docs\esi\E252_CH32V307.xml`）；找不到时 PDO/对象名称退回对象字典原始名称。

## 三、启动后操作顺序

1. 在顶部下拉框选择用于 EtherCAT 的网卡（USB 转以太网卡通常排在最前），点击「连接」；
2. 左侧从站列表选中从站 → 依次 INIT / PREOP / SAFEOP / OP（切 OP 会自动补 PREOP、SAFEOP 步骤）；
3. 「诊断 / EMCY」看 AL 状态码；从站报错时先点「ACK 清错」，再重试状态切换；
4. 「概览」的「过程数据」可看帧间隔、停顿次数、补发帧数、邮箱轮询耗时（用于判断 `0x001B` 是否由主站停顿引起）。

## 四、停止服务

在启动终端按 `Ctrl+C` 即可：先关闭监听端口，再断开主站、关闭网卡句柄并等待周期/邮箱线程退出。

若终端已丢失，按端口反查并结束进程：

```powershell
$owner = (Get-NetTCPConnection -State Listen -LocalPort 8765).OwningProcess
Stop-Process -Id $owner
```

确认端口已释放、无残留进程：

```powershell
Get-NetTCPConnection -State Listen | Where-Object LocalPort -eq 8765   # 应无输出
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'web-ui|main\.py' } |
  Select-Object ProcessId,Name                                        # 应无输出
```

## 五、前置条件与注意

- **Npcap** 必须已安装（pysoem 通过 NPF 访问网卡）；建议以管理员身份运行，否则部分网卡可能无法打开。
- 同一张网卡同时只能有一个主站进程（pysoem 独占打开）；启动前确认没有其他 `python main.py` 或 `slaveinfo.exe` 在跑。
- 后端代码改动需要重启本服务才生效；`web/` 下的静态文件（`app.js`/`style.css`/`index.html`）改完刷新浏览器即可。
- 服务只监听 `127.0.0.1`，不对外网开放。
