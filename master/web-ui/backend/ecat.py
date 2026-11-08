"""EtherCAT 主站服务层：基于 pysoem(SOEM v2.x) 的线程安全调试封装。"""

from __future__ import annotations

import collections
import queue
import threading
import time
import traceback

import pysoem

from .alstatus import explain_al, explain_emcy
from .esi import EsiDictionary

STATE_MAP = {"INIT": 1, "PREOP": 2, "SAFEOP": 4, "OP": 8}
STATE_NAMES = {0: "NONE", 1: "INIT", 2: "PREOP", 4: "SAFEOP", 8: "OP"}

# IO 演示协议：RxPDO byte1..5 固定 00 34 12 78 56（AO1=0x1234，PWM1=0x5678）
DEMO_TAIL = bytes.fromhex("0034127856")

# 周期线程保护：帧间隔超过该值先补发一帧；邮箱轮询独立线程的低频周期
CATCHUP_GAP_MS = 20.0
MBX_POLL_S = 0.2


class EcatError(Exception):
    """面向 UI 的业务异常，消息可直接展示。"""


def state_label(raw: int) -> str:
    base = STATE_NAMES.get(raw & 0x0F, f"0x{raw:02X}")
    return base + ("+ERR" if raw & 0x10 else "")


def _now() -> str:
    return time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"


def _decode_name(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


class EcatService:
    def __init__(self, esi_path: str | None = None):
        self.lock = threading.RLock()
        self._lifecycle = threading.Lock()
        self.master = None
        self.adapter: str | None = None
        self.adapter_desc: str | None = None
        self.connected = False
        self.io_size = 0
        self.esi = EsiDictionary(esi_path)
        self.slaves_meta: list[dict] = []
        self.mapping: dict[int, dict] = {}
        self._thread: threading.Thread | None = None
        self._mbx_thread: threading.Thread | None = None
        self._stop = threading.Event()
        self.cyclic_enabled = True
        self.period_ms = 2.0
        self._last_err_log = 0.0
        self._live = {
            "master_state": 0,
            "master_state_name": "NONE",
            "wkc": 0,
            "expected_wkc": 0,
            # gap_ms: 上一帧间隔；gap_max_ms: 窗口内最大间隔；gap_over50: 超 50 ms 次数；
            # catchup: 因超阈值而补发的帧数（用于判断 0x001B 是否由主站停顿引起）
            "cycle": {"enabled": True, "period_ms": 2.0, "hz": 0.0, "jitter_us": 0.0,
                      "frames": 0, "errors": 0, "gap_ms": 0.0, "gap_max_ms": 0.0,
                      "gap_over50": 0, "catchup": 0,
                      "mbx_ms": 0.0, "mbx_max_ms": 0.0, "mbx_calls": 0},
            "slaves": {},
        }
        self.logs: collections.deque = collections.deque(maxlen=300)
        self.emcy: collections.deque = collections.deque(maxlen=100)
        self.al_history: collections.deque = collections.deque(maxlen=200)
        self._keys: dict[int, dict] = {}
        self.key_events: collections.deque = collections.deque(maxlen=60)
        self.last_demo_cmd: dict | None = None
        # SSE 推送订阅（事件驱动，替代固定 150 ms 快照轮询）
        self._sub_lock = threading.Lock()
        self._subscribers: list[queue.Queue] = []
        self._last_pdo_sig: dict[int, tuple] = {}

    # ---------------- 推送订阅 ----------------
    def subscribe(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=128)
        with self._sub_lock:
            self._subscribers.append(q)
        return q

    def unsubscribe(self, q: queue.Queue):
        with self._sub_lock:
            if q in self._subscribers:
                self._subscribers.remove(q)

    def _broadcast(self, event: str, payload: dict):
        payload = dict(payload)
        payload["ts_ms"] = round(time.time() * 1000, 1)
        with self._sub_lock:
            targets = list(self._subscribers)
        for q in targets:
            try:
                q.put_nowait((event, payload))
            except queue.Full:
                pass

    # ---------------- 基础工具 ----------------
    def _log(self, level: str, msg: str):
        self.logs.append({"t": _now(), "level": level, "msg": msg})

    def _slave(self, pos: int):
        if not self.connected or self.master is None:
            raise EcatError("主站未连接")
        slaves = self.master.slaves
        if pos < 1 or pos > len(slaves):
            raise EcatError(f"从站位置无效: {pos}")
        return slaves[pos - 1]

    def _fmt_exc(self, exc: Exception) -> str:
        code = getattr(exc, "sdo_abort_code", None)
        base = f"{type(exc).__name__}: {exc}".strip()
        if code is not None:
            base += f" (SDO Abort 0x{int(code):08X})"
        return base

    # ---------------- 适配器与连接 ----------------
    @staticmethod
    def list_adapters() -> list[dict]:
        adapters = []
        for a in pysoem.find_adapters():
            adapters.append({"name": a.name, "desc": _decode_name(a.desc)})
        return adapters

    def connect(self, adapter_name: str, adapter_desc: str | None = None):
        # 先停掉可能仍在运行的周期/邮箱线程，再打开新句柄；生命周期操作串行化
        with self._lifecycle:
            self._shutdown()
            with self.lock:
                self._log("info", f"打开网卡 {adapter_name}")
                master = pysoem.Master()
                try:
                    master.open(adapter_name)
                    for attr, value in (("sdo_read_timeout", 1000000), ("sdo_write_timeout", 1000000)):
                        try:
                            setattr(master, attr, value)
                        except Exception:
                            pass
                    wkc = master.config_init()
                    if wkc <= 0:
                        raise EcatError("未扫描到从站（WKC=0）：请检查网线是否插在 IN 口、从站供电以及网卡链路")
                    self._log("info", f"config_init 完成，从站 {len(master.slaves)} 个")
                    io_size = master.config_map()
                except EcatError:
                    try:
                        master.close()
                    except Exception:
                        pass
                    raise
                except Exception as exc:
                    try:
                        master.close()
                    except Exception:
                        pass
                    raise EcatError(f"主站初始化失败: {self._fmt_exc(exc)}")

                self.master = master
                self.adapter = adapter_name
                self.adapter_desc = adapter_desc
                self.connected = True
                self.io_size = io_size
                self._log("info", f"config_map 完成，IO 映射 {io_size} 字节，WKC={wkc}")

                self._live = {
                    "master_state": 0,
                    "master_state_name": "NONE",
                    "wkc": 0,
                    "expected_wkc": 0,
                    "cycle": {"enabled": self.cyclic_enabled, "period_ms": self.period_ms, "hz": 0.0,
                              "jitter_us": 0.0, "frames": 0, "errors": 0, "gap_ms": 0.0,
                              "gap_max_ms": 0.0, "gap_over50": 0, "catchup": 0,
                              "mbx_ms": 0.0, "mbx_max_ms": 0.0, "mbx_calls": 0},
                    "slaves": {i + 1: {"state": 0, "al": 0, "input": [], "output": [], "lost": False} for i in range(len(master.slaves))},
                }
                self.emcy.clear()
                self.al_history.clear()
                self._keys.clear()
                self.key_events.clear()
                self.last_demo_cmd = None
                self._collect_meta()

                self._stop.clear()
                self._thread = threading.Thread(target=self._cyclic_loop, name="ecat-cycle", daemon=True)
                self._thread.start()
                # 邮箱/EMCY 轮询独立线程：不再占用周期帧路径
                self._mbx_thread = threading.Thread(target=self._mbx_loop, name="ecat-mbx", daemon=True)
                self._mbx_thread.start()
                self._log("ok", "主站已连接，周期通信已启动")

    def disconnect(self):
        with self._lifecycle:
            self._shutdown()

    def _shutdown(self):
        """停止周期/邮箱线程并关闭主站句柄。

        先在锁内置 connected=False 并摘掉 self.master 引用，让两个工作线程在下一轮
        立即退出；join 放在锁外，避免工作线程在锁上互等而拖长退出。句柄待线程退出
        后再 close，确保进程退出前网卡已被释放。
        """
        with self.lock:
            was_connected = self.connected
            self._stop.set()
            self.connected = False
            master = self.master
            self.master = None
            thread = self._thread
            mbx = self._mbx_thread
            self._thread = None
            self._mbx_thread = None
        for worker, timeout in ((thread, 1.5), (mbx, 1.5)):
            if worker and worker.is_alive() and worker is not threading.current_thread():
                worker.join(timeout=timeout)
        if master is not None:
            try:
                master.close()
            except Exception:
                pass
        with self.lock:
            if was_connected:
                self._log("info", "主站已断开")
            self.adapter = None
            self.slaves_meta = []
            self.mapping = {}
            self._keys.clear()
            self.key_events.clear()
            self.last_demo_cmd = None
            self._last_pdo_sig.clear()
            self._live["slaves"] = {}
            self._live["master_state_name"] = "NONE"

    # ---------------- 从站静态信息 ----------------
    def _read_sm_regs(self, slave) -> list[dict]:
        result = []
        for n in range(4):
            try:
                raw = slave._fprd(0x0800 + n * 8, 8)
                result.append({
                    "index": n,
                    "start": int.from_bytes(raw[0:2], "little"),
                    "length": int.from_bytes(raw[2:4], "little"),
                    "control": raw[4],
                    "status": raw[5],
                    "activate": raw[6],
                    "pdi": raw[7],
                })
            except Exception:
                result.append({"index": n, "error": "寄存器读取失败"})
        return result

    def _read_fmmu_regs(self, slave, count: int = 3) -> list[dict]:
        result = []
        for n in range(count):
            try:
                raw = slave._fprd(0x0C00 + n * 16, 16)
                result.append({
                    "index": n,
                    "logical_start": int.from_bytes(raw[0:4], "little"),
                    "length": int.from_bytes(raw[4:6], "little"),
                    "logical_start_bit": raw[6],
                    "logical_end_bit": raw[7],
                    "phys_start": int.from_bytes(raw[8:10], "little"),
                    "phys_start_bit": raw[10],
                    "type": raw[11],
                    "activate": raw[12],
                })
            except Exception:
                result.append({"index": n, "error": "寄存器读取失败（该 ESC/固件组合下 FMMU 区域可能不可读）"})
        return result

    def _read_pdo_assign(self, slave, index: int) -> list[int]:
        count = int.from_bytes(slave.sdo_read(index, 0), "little")
        result = []
        for sub in range(1, count + 1):
            result.append(int.from_bytes(slave.sdo_read(index, sub), "little"))
        return result

    def _read_pdo_map(self, slave, pdo_index: int) -> dict:
        count = int.from_bytes(slave.sdo_read(pdo_index, 0), "little")
        entries = []
        bit = 0
        for sub in range(1, count + 1):
            raw = int.from_bytes(slave.sdo_read(pdo_index, sub), "little")
            obj_index = raw >> 16
            obj_sub = (raw >> 8) & 0xFF
            bits = raw & 0xFF
            name = ""
            if obj_index == 0:
                name = "填充位"
            else:
                name = self.esi.signal_name(obj_index, obj_sub)
            if not name:
                name = f"0x{obj_index:04X}:{obj_sub:02X}"
            entries.append({
                "index": obj_index,
                "sub": obj_sub,
                "bits": bits,
                "bit_offset": bit,
                "name": name,
                "type": self.esi.describe(obj_index, obj_sub).get("type", ""),
            })
            bit += bits
        return {"pdo": pdo_index, "entries": entries, "bits": bit}

    def _collect_meta(self):
        master = self.master
        assert master is not None
        self.slaves_meta = []
        self.mapping = {}
        for pos, slave in enumerate(master.slaves, 1):
            meta = {
                "pos": pos,
                "sii": {
                    "name": _decode_name(slave.name),
                    "man": slave.man,
                    "id": slave.id,
                    "rev": slave.rev,
                },
                "identity": None,
                "in_size": 0,
                "out_size": 0,
                "sm": [],
                "fmmu": [],
                "pdo_assign": {"rx": [], "tx": []},
                "sync": {},
                "mismatch": False,
                "mapping_error": None,
            }
            try:
                meta["in_size"] = len(slave.input)
                meta["out_size"] = len(slave.output)
            except Exception:
                pass
            try:
                vid = int.from_bytes(slave.sdo_read(0x1018, 1), "little")
                pid = int.from_bytes(slave.sdo_read(0x1018, 2), "little")
                rev = int.from_bytes(slave.sdo_read(0x1018, 3), "little")
                meta["identity"] = {"vendor": vid, "product": pid, "revision": rev}
                meta["mismatch"] = (vid != meta["sii"]["man"] or pid != meta["sii"]["id"] or rev != meta["sii"]["rev"])
            except Exception as exc:
                meta["identity"] = {"error": self._fmt_exc(exc)}
            try:
                meta["sm"] = self._read_sm_regs(slave)
                meta["fmmu"] = self._read_fmmu_regs(slave)
            except Exception:
                pass
            # 实际 PDO 映射（以对象字典 0x1C12/0x1C13 为准）
            rx, tx = [], []
            try:
                for pdo in self._read_pdo_assign(slave, 0x1C12):
                    rx.append(self._read_pdo_map(slave, pdo))
                for pdo in self._read_pdo_assign(slave, 0x1C13):
                    tx.append(self._read_pdo_map(slave, pdo))
                meta["pdo_assign"] = {"rx": [p["pdo"] for p in rx], "tx": [p["pdo"] for p in tx]}
                self.mapping[pos] = {
                    "rx": {"pdos": rx, "bits": sum(p["bits"] for p in rx)},
                    "tx": {"pdos": tx, "bits": sum(p["bits"] for p in tx)},
                }
            except Exception as exc:
                meta["mapping_error"] = self._fmt_exc(exc)
                self.mapping[pos] = {"rx": {"pdos": [], "bits": 0}, "tx": {"pdos": [], "bits": 0}}
            try:
                meta["sync"]["output_type"] = int.from_bytes(slave.sdo_read(0x1C32, 1), "little")
                meta["sync"]["input_type"] = int.from_bytes(slave.sdo_read(0x1C33, 1), "little")
                meta["sync"]["min_cycle"] = int.from_bytes(slave.sdo_read(0x1C32, 5), "little")
            except Exception:
                pass
            self.slaves_meta.append(meta)

    # ---------------- 周期通信线程 ----------------
    def _track_key(self, pos: int, data: list[int]):
        """跟踪 TxPDO byte0 bit0（USER 按键，1=按下）。"""
        bit = (int(data[0]) & 0x01) if data else 0
        info = self._keys.get(pos)
        if info is None:
            self._keys[pos] = {"bit": bit, "presses": 0, "edges": 0, "last_change": "—", "last_edge": "—"}
            return
        if info["bit"] == bit:
            return
        info["bit"] = bit
        info["edges"] += 1
        info["last_change"] = _now()
        info["last_edge"] = "按下" if bit else "松开"
        if bit:
            info["presses"] += 1
        edge = "按下" if bit else "松开"
        self.key_events.append({"t": info["last_change"], "pos": pos, "bit": bit, "edge": edge})
        self._log("ok" if bit else "info", f"从站 {pos} USER 按键{edge}")
        self._broadcast("key", {
            "pos": pos,
            "bit": bit,
            "edge": edge,
            "t": info["last_change"],
            "presses": info["presses"],
            "edges": info["edges"],
            "input": list(data),
        })

    def _cyclic_loop(self):
        master = self.master
        last_state_poll = 0.0
        last_t = None
        last_send_t = None
        last_gap_log = 0.0
        hz_ema = 0.0
        jitter_ema = 0.0

        while not self._stop.is_set():
            if not self.cyclic_enabled:
                time.sleep(0.03)
                continue
            t_begin = time.perf_counter()
            # 帧间隔：自上一帧发出到本帧开始的间隔，用于判断主站是否停顿
            gap_ms = (t_begin - last_send_t) * 1000.0 if last_send_t else 0.0
            wkc = 0
            try:
                with self.lock:
                    if not self.connected or self.master is None:
                        break
                    if gap_ms > CATCHUP_GAP_MS:
                        # 停顿后先补发一帧，缩短从站 SM 看门狗看到的空窗
                        try:
                            master.send_processdata()
                            master.receive_processdata(1000)
                            self._live["cycle"]["catchup"] += 1
                        except Exception:
                            pass
                    master.send_processdata()
                    result = master.receive_processdata(1000)
                    wkc = int(result) if isinstance(result, (int, float)) else master.expected_wkc
                    for pos, slave in enumerate(master.slaves, 1):
                        live = self._live["slaves"].setdefault(pos, {"state": 0, "al": 0, "input": [], "output": [], "lost": False})
                        try:
                            live["input"] = list(slave.input)
                            live["output"] = list(slave.output)
                            live["lost"] = bool(slave.is_lost)
                        except Exception:
                            pass
                        self._track_key(pos, live.get("input", []))
                        # 输入 byte0 / 输出整帧变化时立即推送（不等下一次快照）
                        sig = (tuple(live.get("input", [])[:1]), tuple(live.get("output", [])))
                        if self._last_pdo_sig.get(pos) != sig:
                            self._last_pdo_sig[pos] = sig
                            self._broadcast("pdo", {
                                "pos": pos,
                                "input": list(live.get("input", [])),
                                "output": list(live.get("output", [])),
                                "state": live.get("state", 0),
                                "al": live.get("al", 0),
                            })
                    self._live["wkc"] = wkc
                    self._live["expected_wkc"] = master.expected_wkc
            except Exception as exc:
                now = time.time()
                if now - self._last_err_log > 2.0:
                    self._last_err_log = now
                    self._log("warn", f"周期通信异常: {self._fmt_exc(exc)}")
                    self._live["cycle"]["errors"] += 1
                time.sleep(0.02)
                continue

            now = time.perf_counter()
            last_send_t = now
            period = now - t_begin
            if last_t is not None:
                dt = now - last_t
                inst_hz = 1.0 / dt if dt > 0 else 0.0
                hz_ema = inst_hz if hz_ema == 0 else hz_ema * 0.9 + inst_hz * 0.1
                jitter = abs(period * 1000.0 - self.period_ms) * 1000.0
                jitter_ema = jitter if jitter_ema == 0 else jitter_ema * 0.9 + jitter * 0.1
            last_t = now
            cycle = self._live["cycle"]
            if gap_ms > 50.0:
                cycle["gap_over50"] = cycle.get("gap_over50", 0) + 1
                if now - last_gap_log > 2.0:
                    last_gap_log = now
                    self._log("warn", f"周期帧间隔 {gap_ms:.1f} ms（>50 ms），已补发一帧")
            cycle["gap_ms"] = round(gap_ms, 2)
            if gap_ms > cycle.get("gap_max_ms", 0.0):
                cycle["gap_max_ms"] = round(gap_ms, 2)
            cycle.update({
                "enabled": True,
                "period_ms": self.period_ms,
                "hz": round(hz_ema, 1),
                "jitter_us": round(jitter_ema, 1),
                "frames": cycle["frames"] + 1,
            })

            if now - last_state_poll > 0.2:
                last_state_poll = now
                try:
                    with self.lock:
                        if self.connected and self.master is not None:
                            lowest = self.master.read_state()
                            self._live["master_state"] = lowest
                            self._live["master_state_name"] = state_label(lowest)
                            for pos, slave in enumerate(self.master.slaves, 1):
                                live = self._live["slaves"].setdefault(pos, {"state": 0, "al": 0, "input": [], "output": [], "lost": False})
                                prev_al = live.get("al")
                                prev_state = live.get("state")
                                try:
                                    raw_state = int(slave.state)
                                    al = int(slave.al_status)
                                except Exception:
                                    continue
                                live["state"] = raw_state
                                live["al"] = al
                                if (prev_state is not None and prev_state != raw_state) or \
                                        (prev_al is not None and prev_al != al):
                                    self._broadcast("pdo", {
                                        "pos": pos,
                                        "input": list(live.get("input", [])),
                                        "output": list(live.get("output", [])),
                                        "state": raw_state,
                                        "al": al,
                                    })
                                if prev_al is not None and prev_al != al:
                                    self.al_history.append({
                                        "t": _now(), "pos": pos, "state": raw_state,
                                        "state_name": state_label(raw_state), "al": al,
                                        "al_text": explain_al(al)["name"],
                                    })
                                    if al != 0:
                                        self._log("warn", f"从站 {pos} AL 状态码变化: 0x{al:04X} {explain_al(al)['name']}")
                except Exception:
                    pass

            elapsed = time.perf_counter() - now
            remaining = self.period_ms / 1000.0 - elapsed
            if remaining > 0:
                time.sleep(remaining)

    def _mbx_loop(self):
        """邮箱/EMCY 轮询：独立低频线程，不再占用周期帧路径。

        仅在总线健康（WKC 达标）时轮询，避免从站异常时 mbx_receive 长时间阻塞并把
        周期线程拖到 SM 看门狗超时（100 ms）。
        """
        while not self._stop.is_set():
            time.sleep(MBX_POLL_S)
            # 用带超时的抢锁，避免断开时 join 被阻塞
            if not self.lock.acquire(timeout=0.2):
                continue
            try:
                if not self.connected or self.master is None or not self.cyclic_enabled:
                    continue
                expected = self._live.get("expected_wkc", 0)
                if expected and self._live.get("wkc", 0) != expected:
                    continue
                t_mbx = time.perf_counter()
                for pos, slave in enumerate(self.master.slaves, 1):
                    try:
                        slave.mbx_receive()
                    except pysoem.Emergency as em:
                        item = {
                            "t": _now(),
                            "pos": getattr(em, "slave_pos", pos),
                            "code": getattr(em, "error_code", 0),
                            "reg": getattr(em, "error_reg", 0),
                            "b1": getattr(em, "b1", 0),
                            "w1": getattr(em, "w1", 0),
                            "w2": getattr(em, "w2", 0),
                        }
                        item["text"] = explain_emcy(item["code"])
                        self.emcy.append(item)
                        self._log("warn", f"从站 {item['pos']} EMCY 0x{item['code']:04X}: {item['text']}")
                    except Exception:
                        pass
                # 记录单次邮箱轮询耗时：它是周期锁内唯一可能长时间阻塞的调用，
                # 暴露到界面后可直接判断 0x001B 是否由主站邮箱停顿引起。
                cycle = self._live["cycle"]
                cost = (time.perf_counter() - t_mbx) * 1000.0
                cycle["mbx_ms"] = round(cost, 2)
                if cost > cycle.get("mbx_max_ms", 0.0):
                    cycle["mbx_max_ms"] = round(cost, 2)
                cycle["mbx_calls"] = cycle.get("mbx_calls", 0) + 1
            finally:
                self.lock.release()

    # ---------------- 状态机 ----------------
    def _wait_for_state(self, target: int, timeout_s: float) -> tuple[bool, int, list[dict]]:
        master = self.master
        assert master is not None
        deadline = time.perf_counter() + timeout_s
        lowest = 0
        while time.perf_counter() < deadline:
            try:
                current = master.read_state()
                lowest = current
                if current >= target:
                    return True, current, self._state_details()
            except Exception:
                pass
            # 状态切换期间必须持续喂过程数据（OP 前要求至少一帧，且 SM2 看门狗需保持）
            try:
                master.send_processdata()
                master.receive_processdata(1000)
            except Exception:
                pass
            time.sleep(0.002)
        return False, lowest, self._state_details()

    def _state_details(self) -> list[dict]:
        details = []
        for pos, slave in enumerate(self.master.slaves, 1):
            try:
                details.append({"pos": pos, "state": int(slave.state), "al": int(slave.al_status)})
            except Exception:
                details.append({"pos": pos, "state": -1, "al": -1})
        return details

    def request_state(self, target_name: str, timeout_s: float = 5.0) -> dict:
        target = STATE_MAP.get(target_name.upper())
        if target is None:
            raise EcatError(f"未知目标状态: {target_name}")
        with self.lock:
            if not self.connected or self.master is None:
                raise EcatError("主站未连接")
            master = self.master
            try:
                current = master.read_state()
            except Exception as exc:
                raise EcatError(f"读取当前状态失败: {self._fmt_exc(exc)}")

            steps: list[int] = []
            if target >= 4 and (current & 0x0F) < 2:
                steps.append(2)
            if target >= 8 and (current & 0x0F) < 4:
                steps.append(4)
            steps.append(target)

            for step in steps:
                self._log("info", f"请求状态 {STATE_NAMES[step]}")
                # OP 前先发至少一帧过程数据（从站 OP_PD_REQUIRED=1）
                try:
                    master.send_processdata()
                    master.receive_processdata(1000)
                except Exception:
                    pass
                try:
                    master.state = step
                    master.write_state()
                except Exception as exc:
                    raise EcatError(f"下发 {STATE_NAMES[step]} 请求失败: {self._fmt_exc(exc)}")
                ok, lowest, details = self._wait_for_state(step, timeout_s)
                if not ok:
                    codes = ", ".join(f"S{ d['pos']}:{state_label(d['state'])}/AL=0x{d['al']:04X}" for d in details)
                    al_code = details[0]["al"] if details else 0
                    hint = explain_al(al_code)
                    raise EcatError(
                        f"切换到 {STATE_NAMES[step]} 失败（当前最低状态 {state_label(lowest)}，{codes}）。"
                        f"AL 0x{al_code:04X} {hint['name']}：{hint['detail']}"
                    )
            self._log("ok", f"已进入 {target_name.upper()}")
            return {"state": master.read_state(), "slaves": self._state_details()}

    def ack_state(self) -> dict:
        """AL Control = 当前状态 | ACK：清除从站错误位，并记录前后原始状态码。"""
        with self.lock:
            if not self.connected or self.master is None:
                raise EcatError("主站未连接")
            master = self.master
            before = self._state_details()
            try:
                current = master.read_state()
                master.state = (int(current) & 0x0F) | 0x10
                master.write_state()
            except Exception as exc:
                raise EcatError(f"ACK 清错失败: {self._fmt_exc(exc)}")
            # 等待 ACK 生效期间必须持续喂过程数据，否则 SM2 100 ms 看门狗会立即再触发 0x001B
            deadline = time.perf_counter() + 0.15
            while time.perf_counter() < deadline:
                try:
                    master.send_processdata()
                    master.receive_processdata(1000)
                    master.read_state()
                except Exception:
                    pass
                time.sleep(0.002)
            after = self._state_details()
            for d in before:
                self._log("warn" if d["al"] else "info",
                          f"ACK 前 S{d['pos']}: {state_label(d['state'])} AL=0x{d['al']:04X}")
            for d in after:
                self._log("info", f"ACK 后 S{d['pos']}: {state_label(d['state'])} AL=0x{d['al']:04X}")
            return {"before": before, "after": after}

    # ---------------- SDO ----------------
    def sdo_read(self, pos: int, index: int, sub: int, size: int | None = None) -> dict:
        with self.lock:
            slave = self._slave(pos)
            try:
                data = slave.sdo_read(index, sub, size) if size else slave.sdo_read(index, sub)
            except Exception as exc:
                raise EcatError(self._fmt_exc(exc))
            return {"index": index, "sub": sub, "length": len(data), "data": list(data)}

    def sdo_write(self, pos: int, index: int, sub: int, data: bytes) -> dict:
        with self.lock:
            slave = self._slave(pos)
            try:
                slave.sdo_write(index, sub, data)
            except Exception as exc:
                raise EcatError(self._fmt_exc(exc))
            self._log("info", f"SDO 写入 {index:#06X}:{sub:02X} <- {data.hex(' ')}")
            return {"ok": True, "index": index, "sub": sub, "length": len(data)}

    def read_object_values(self, pos: int, index: int) -> dict:
        with self.lock:
            slave = self._slave(pos)
            values = {}
            try:
                count = int.from_bytes(slave.sdo_read(index, 0), "little")
            except Exception:
                count = 0
            subs = [0]
            if index in self.esi.objects:
                for (idx, sub) in self.esi.subs:
                    if idx == index and sub not in subs:
                        subs.append(sub)
            if count:
                for sub in range(1, count + 1):
                    if sub not in subs:
                        subs.append(sub)
            for sub in sorted(subs):
                try:
                    data = slave.sdo_read(index, sub)
                    values[str(sub)] = {"length": len(data), "data": list(data)}
                except Exception as exc:
                    values[str(sub)] = {"error": self._fmt_exc(exc)}
            return {"index": index, "values": values}

    def od(self, pos: int) -> dict:
        with self.lock:
            slave = self._slave(pos)
            try:
                raw_od = slave.od
            except Exception as exc:
                raise EcatError(f"读取对象字典失败: {self._fmt_exc(exc)}")
            objects = []
            for item in raw_od:
                index = int(item.index)
                esi_obj = self.esi.objects.get(index, {})
                entries = []
                for position, entry in enumerate(item.entries):
                    entries.append({
                        "sub": position,
                        "name": _decode_name(entry.name),
                        "data_type": int(entry.data_type),
                        "bits": int(entry.bit_length),
                        "access": int(entry.obj_access),
                    })
                objects.append({
                    "index": index,
                    "hex": f"0x{index:04X}",
                    "name": _decode_name(item.name),
                    "esi_name": esi_obj.get("name", ""),
                    "object_code": int(item.object_code),
                    "data_type": int(item.data_type),
                    "bits": int(item.bit_length),
                    "access": esi_obj.get("access", ""),
                    "entries": entries,
                    "subs": sorted(s for (idx, s) in self.esi.subs if idx == index),
                    "sub_info": [
                        {
                            "sub": sub,
                            "name": info.get("name", ""),
                            "type": info.get("type", ""),
                            "bits": info.get("bits", 0),
                            "access": info.get("access", ""),
                        }
                        for (idx, sub), info in sorted(self.esi.subs.items())
                        if idx == index
                    ],
                })
            objects.sort(key=lambda o: o["index"])
            return {"objects": objects, "count": len(objects)}

    # ---------------- 过程数据 ----------------
    def _decode(self, raw: list[int], entries: list[dict]) -> list[dict]:
        result = []
        for ent in entries:
            bits = ent["bits"]
            off = ent["bit_offset"]
            value = 0
            for i in range(bits):
                p = off + i
                if p // 8 >= len(raw):
                    break
                if (raw[p // 8] >> (p % 8)) & 1:
                    value |= 1 << i
            signed = "INT" in ent.get("type", "").upper() and "UINT" not in ent.get("type", "").upper()
            if signed and bits > 0 and value & (1 << (bits - 1)):
                value -= 1 << bits
            result.append({
                "name": ent["name"],
                "index": ent["index"],
                "sub": ent["sub"],
                "bits": bits,
                "bit_offset": off,
                "value": value,
                "bool": bool(value & 1) if bits == 1 else None,
            })
        return result

    def pdo_mapping(self, pos: int) -> dict:
        if pos not in self.mapping:
            raise EcatError(f"从站 {pos} 无映射信息")
        mapping = self.mapping[pos]
        # 补充数据类型（来自 ESI），用于有符号解析
        for direction in ("rx", "tx"):
            for pdo in mapping[direction]["pdos"]:
                for ent in pdo["entries"]:
                    ent["type"] = self.esi.describe(ent["index"], ent["sub"]).get("type", "")
        return mapping

    def write_pdo_field(self, pos: int, bit_offset: int, bit_length: int, value: int) -> dict:
        with self.lock:
            slave = self._slave(pos)
            try:
                data = bytearray(slave.output)
            except Exception as exc:
                raise EcatError(f"读取输出缓冲失败: {self._fmt_exc(exc)}")
            if bit_offset + bit_length > len(data) * 8:
                raise EcatError("字段超出输出缓冲范围")
            for i in range(bit_length):
                p = bit_offset + i
                if (value >> i) & 1:
                    data[p // 8] |= 1 << (p % 8)
                else:
                    data[p // 8] &= ~(1 << (p % 8))
            try:
                slave.output = bytes(data)
            except Exception as exc:
                raise EcatError(f"写输出缓冲失败: {self._fmt_exc(exc)}")
            self._log("info", f"从站 {pos} 输出字段写入 bit{bit_offset}+{bit_length} = {value}")
            return {"output": list(data)}

    def write_pdo_raw(self, pos: int, data: list[int]) -> dict:
        with self.lock:
            slave = self._slave(pos)
            try:
                current = len(slave.output)
                payload = bytes(data)
                if len(payload) < current:
                    payload += bytes(current - len(payload))
                slave.output = payload[:current]
            except Exception as exc:
                raise EcatError(f"写输出缓冲失败: {self._fmt_exc(exc)}")
            self._log("info", f"从站 {pos} 输出原始数据 <- {bytes(data).hex(' ')}")
            return {"output": list(slave.output)}

    def demo_output(self, pos: int, byte0: int) -> dict:
        """IO 演示协议：byte0 命令 + byte1..5 固定 00 34 12 78 56。"""
        with self.lock:
            slave = self._slave(pos)
            if not 0 <= byte0 <= 0xFF:
                raise EcatError("byte0 必须在 0..255 之间")
            try:
                size = len(slave.output)
                data = bytearray(slave.output)
            except Exception as exc:
                raise EcatError(f"读取输出缓冲失败: {self._fmt_exc(exc)}")
            if size < 6:
                raise EcatError(f"输出缓冲仅 {size} 字节，与演示协议 6 字节不符")
            data[0] = byte0
            data[1:6] = DEMO_TAIL
            try:
                slave.output = bytes(data)
            except Exception as exc:
                raise EcatError(f"写输出缓冲失败: {self._fmt_exc(exc)}")
            try:
                in_op = (int(slave.state) & 0x0F) >= 8
            except Exception:
                in_op = False
            if not in_op:
                self._log("warn", "当前未处于 OP，演示输出不会发送到从站")
            self.last_demo_cmd = {"pos": pos, "byte0": byte0, "t": _now()}
            self._log("info", f"从站 {pos} 演示命令 byte0={byte0:#04x} -> {bytes(data).hex(' ')}")
            return {"output": list(data), "byte0": byte0, "in_op": in_op}

    def set_cycle(self, enabled: bool | None = None, period_ms: float | None = None):
        with self.lock:
            if enabled is not None:
                self.cyclic_enabled = bool(enabled)
                self._live["cycle"]["enabled"] = bool(enabled)
            if period_ms is not None:
                value = max(0.5, min(50.0, float(period_ms)))
                self.period_ms = value
                self._live["cycle"]["period_ms"] = value
            self._log("info", f"周期设置: {'运行' if self.cyclic_enabled else '暂停'} / {self.period_ms} ms")
            return {"enabled": self.cyclic_enabled, "period_ms": self.period_ms}

    # ---------------- 快照 ----------------
    def slave_detail(self, pos: int) -> dict:
        with self.lock:
            slave = self._slave(pos)
            meta = next((m for m in self.slaves_meta if m["pos"] == pos), {})
            regs = {}
            for name, addr, size in (("dl_status", 0x0100, 2), ("al_control", 0x0120, 2), ("al_status", 0x0130, 2), ("al_status_code", 0x0134, 2)):
                try:
                    regs[name] = {"addr": addr, "data": list(slave._fprd(addr, size))}
                except Exception as exc:
                    regs[name] = {"addr": addr, "error": self._fmt_exc(exc)}
            meta["regs"] = regs
            meta["esi_device"] = {
                "name": self.esi.device_name,
                "vendor": self.esi.vendor_id,
                "product": self.esi.product_code,
                "revision": self.esi.revision,
            }
            return meta

    def snapshot(self) -> dict:
        with self.lock:
            connected = self.connected
            adapter = self.adapter
            adapter_desc = self.adapter_desc
            io_size = self.io_size
            live = {
                "master_state": self._live["master_state"],
                "master_state_name": self._live["master_state_name"],
                "wkc": self._live["wkc"],
                "expected_wkc": self._live["expected_wkc"],
                "cycle": dict(self._live["cycle"]),
            }
            slaves_live = {k: dict(v) for k, v in self._live["slaves"].items()}
            logs = list(self.logs)[-60:]
            emcy = list(self.emcy)[-20:]
            al_history = list(self.al_history)[-20:]
            demo = {
                "last": dict(self.last_demo_cmd) if self.last_demo_cmd else None,
                "keys": {str(pos): dict(info) for pos, info in self._keys.items()},
                "key_events": list(self.key_events),
            }
            mapping = self.mapping
            meta_list = self.slaves_meta

        slaves = []
        for meta in meta_list:
            pos = meta["pos"]
            state = slaves_live.get(pos, {})
            mapping_pos = mapping.get(pos, {})
            in_entries = [e for pdo in mapping_pos.get("tx", {}).get("pdos", []) for e in pdo["entries"]]
            out_entries = [e for pdo in mapping_pos.get("rx", {}).get("pdos", []) for e in pdo["entries"]]
            al = int(state.get("al", 0))
            slaves.append({
                "pos": pos,
                "sii": meta["sii"],
                "identity": meta["identity"],
                "mismatch": meta["mismatch"],
                "state": state.get("state", 0),
                "state_name": state_label(int(state.get("state", 0))),
                "al": al,
                "al_explain": explain_al(al),
                "lost": state.get("lost", False),
                "in_size": meta["in_size"],
                "out_size": meta["out_size"],
                "input": state.get("input", []),
                "output": state.get("output", []),
                "inputs": self._decode(state.get("input", []), in_entries),
                "outputs": self._decode(state.get("output", []), out_entries),
            })

        return {
            "connected": connected,
            "ts_ms": round(time.time() * 1000, 1),
            "adapter": adapter,
            "adapter_desc": adapter_desc,
            "io_size": io_size,
            "live": live,
            "slaves": slaves,
            "logs": logs,
            "emcy": [dict(e) for e in emcy],
            "al_history": al_history,
            "demo": demo,
            "esi_error": self.esi.error,
        }
