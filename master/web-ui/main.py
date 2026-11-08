"""E252 EtherCAT 主站调试台 —— 启动入口。

用法：
    python main.py            # 默认 http://127.0.0.1:8765
    python main.py --port 9000 --no-browser
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys
import threading
import webbrowser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from backend.ecat import EcatService
from backend.server import create_server


def find_esi() -> str | None:
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    candidate = os.path.join(root, "docs", "esi", "E252_CH32V307.xml")
    return candidate if os.path.isfile(candidate) else None


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return True


def main():
    parser = argparse.ArgumentParser(description="E252 EtherCAT 主站调试台")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址，默认 127.0.0.1")
    parser.add_argument("--port", type=int, default=8765, help="监听端口，默认 8765")
    parser.add_argument("--esi", default=None, help="ESI XML 路径，默认使用 docs/esi/E252_CH32V307.xml")
    parser.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    args = parser.parse_args()

    esi_path = args.esi or find_esi()
    service = EcatService(esi_path)
    try:
        httpd = create_server(args.host, args.port, service)
    except OSError as exc:
        print(f"[ERROR] 端口 {args.port} 无法监听: {exc}\n        可尝试: python main.py --port 8790")
        raise SystemExit(1)

    url = f"http://{args.host}:{args.port}/"
    print("=" * 64)
    print(" E252 EtherCAT 主站调试台")
    print(f" 界面地址 : {url}")
    print(f" ESI 文件 : {esi_path or '未找到（PDO/对象名称将退回对象字典原始名称）'}")
    print(f" 权限     : {'管理员' if is_admin() else '普通用户（Npcap 抓包可能需要管理员权限）'}")
    print(" 说明     : 先在界面选择用于 EtherCAT 的网卡并点击“连接”，Ctrl+C 停止并释放网卡")
    print("=" * 64)

    if not args.no_browser:
        timer = threading.Timer(1.0, lambda: webbrowser.open(url))
        timer.daemon = True
        timer.start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n正在退出…")
    finally:
        try:
            httpd.server_close()
        finally:
            service.disconnect()


if __name__ == "__main__":
    main()
