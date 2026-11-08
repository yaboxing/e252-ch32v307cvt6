"""本地 HTTP 服务：REST + SSE，驱动浏览器界面。"""

from __future__ import annotations

import json
import mimetypes
import os
import queue
import re
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from .ecat import EcatError, EcatService

WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")


class ApiHandler(BaseHTTPRequestHandler):
    server_version = "E252EcatMasterUI/1.0"
    service: EcatService  # 由 create_server 注入

    # ---------- 基础输出 ----------
    def _send_json(self, payload, status: int = 200):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _ok(self, payload=None):
        data = {"ok": True}
        if payload:
            data.update(payload)
        self._send_json(data)

    def _error(self, message: str, status: int = 400):
        self._send_json({"ok": False, "error": message}, status)

    def log_message(self, fmt, *args):
        # 静默 HTTP 访问日志，避免刷屏
        pass

    def _read_json(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    @staticmethod
    def _int_arg(value, default=None):
        if value is None:
            return default
        text = str(value).strip()
        try:
            return int(text, 16) if text.lower().startswith(("0x", "#x")) else int(text, 10)
        except ValueError:
            raise EcatError(f"参数不是合法整数: {value}")

    # ---------- 路由 ----------
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)
        try:
            if path == "/api/adapters":
                return self._ok({"adapters": EcatService.list_adapters()})
            if path == "/api/snapshot":
                return self._ok(self.service.snapshot())
            if path == "/api/events":
                return self._stream_events()
            if path == "/api/sdo":
                return self._handle_sdo_read(query)
            if path == "/api/emcy/clear":
                self.service.emcy.clear()
                return self._ok()
            match = re.match(r"^/api/slaves/(\d+)$", path)
            if match:
                return self._ok(self.service.slave_detail(int(match.group(1))))
            match = re.match(r"^/api/slaves/(\d+)/od$", path)
            if match:
                return self._ok(self.service.od(int(match.group(1))))
            match = re.match(r"^/api/slaves/(\d+)/mapping$", path)
            if match:
                return self._ok(self.service.pdo_mapping(int(match.group(1))))
            match = re.match(r"^/api/slaves/(\d+)/object/(\d+)$", path)
            if match:
                pos = int(match.group(1))
                index = self._int_arg(match.group(2))
                return self._ok(self.service.read_object_values(pos, index))
            return self._serve_static(path)
        except EcatError as exc:
            self._error(str(exc))
        except Exception as exc:
            traceback.print_exc()
            self._error(f"{type(exc).__name__}: {exc}", 500)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        body = self._read_json()
        try:
            if path == "/api/connect":
                adapter = body.get("adapter")
                if not adapter:
                    return self._error("缺少 adapter 参数")
                self.service.connect(adapter, body.get("desc"))
                return self._ok(self.service.snapshot() | {"adapters": EcatService.list_adapters()})
            if path == "/api/disconnect":
                self.service.disconnect()
                return self._ok(self.service.snapshot())
            if path == "/api/state":
                target = body.get("state", "")
                timeout = float(body.get("timeout", 5.0))
                return self._ok(self.service.request_state(target, timeout))
            if path == "/api/state/ack":
                return self._ok(self.service.ack_state())
            if path == "/api/sdo/write":
                pos = int(body["pos"])
                index = self._int_arg(body["index"])
                sub = self._int_arg(body.get("sub", 0))
                data = body.get("data", [])
                if isinstance(data, str):
                    cleaned = data.replace(" ", "").replace(",", "")
                    if len(cleaned) % 2:
                        raise EcatError("十六进制数据长度必须为偶数")
                    payload = bytes.fromhex(cleaned)
                else:
                    payload = bytes(int(x) & 0xFF for x in data)
                result = self.service.sdo_write(pos, index, sub, payload)
                return self._ok(result)
            if path == "/api/cycle":
                enabled = body.get("enabled")
                period = body.get("period_ms")
                return self._ok(self.service.set_cycle(enabled, float(period) if period is not None else None))
            if path == "/api/pdo/write":
                return self._ok(self.service.write_pdo_field(
                    int(body["pos"]), int(body["bit_offset"]), int(body["bit_length"]), int(body.get("value", 0))
                ))
            if path == "/api/pdo/raw":
                return self._ok(self.service.write_pdo_raw(int(body["pos"]), list(body.get("data", []))))
            if path == "/api/demo/output":
                pos = int(body["pos"])
                byte0 = self._int_arg(body.get("byte0", 0))
                return self._ok(self.service.demo_output(pos, byte0))
            return self._error(f"未知接口: {path}", 404)
        except EcatError as exc:
            self._error(str(exc))
        except KeyError as exc:
            self._error(f"缺少参数: {exc}")
        except Exception as exc:
            traceback.print_exc()
            self._error(f"{type(exc).__name__}: {exc}", 500)

    # ---------- 具体处理 ----------
    def _handle_sdo_read(self, query):
        pos = self._int_arg(query.get("pos", ["1"])[0])
        index = self._int_arg(query.get("index", [None])[0])
        sub = self._int_arg(query.get("sub", ["0"])[0])
        size = self._int_arg(query.get("size", [None])[0])
        if index is None:
            return self._error("缺少 index 参数")
        return self._ok(self.service.sdo_read(pos, index, sub, size))

    def _sse_write(self, event: str, payload: dict):
        chunk = f"event: {event}\ndata: {json.dumps(payload, ensure_ascii=False)}\n\n"
        self.wfile.write(chunk.encode("utf-8"))
        self.wfile.flush()

    def _stream_events(self):
        """事件驱动推送：key/pdo 变化即时下发，另以 1 s 快照兜底。"""
        sub = self.service.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        try:
            self.wfile.write(b"retry: 1500\n\n")
            self.wfile.flush()
            self._sse_write("snapshot", self.service.snapshot())
            last_snap = time.time()
            last_beat = last_snap
            while True:
                try:
                    event, payload = sub.get(timeout=0.05)
                except queue.Empty:
                    event = None
                if event:
                    self._sse_write(event, payload)
                now = time.time()
                if now - last_snap >= 1.0:
                    self._sse_write("snapshot", self.service.snapshot())
                    last_snap = last_beat = now
                elif now - last_beat >= 15.0:
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
                    last_beat = now
        except (BrokenPipeError, ConnectionResetError, OSError):
            return
        finally:
            self.service.unsubscribe(sub)

    def _serve_static(self, path: str):
        if path in ("", "/"):
            path = "/index.html"
        web_abs = os.path.abspath(WEB_DIR)
        file_path = os.path.abspath(os.path.normpath(os.path.join(web_abs, path.lstrip("/"))))
        if not file_path.startswith(web_abs + os.sep) or not os.path.isfile(file_path):
            self._error("Not found", 404)
            return
        mime, _ = mimetypes.guess_type(file_path)
        ctype = mime or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
            ctype += "; charset=utf-8"
        with open(file_path, "rb") as handle:
            body = handle.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def create_server(host: str, port: int, service: EcatService) -> ThreadingHTTPServer:
    handler = type("BoundApiHandler", (ApiHandler,), {"service": service})
    httpd = ThreadingHTTPServer((host, port), handler)
    httpd.daemon_threads = True
    return httpd
