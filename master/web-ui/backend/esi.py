"""解析 ESI（EtherCAT Slave Information）XML，为对象字典与 PDO 提供名称/类型注释。"""

from __future__ import annotations

import xml.etree.ElementTree as ET


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _parse_int(text: str, default: int = 0) -> int:
    if not text:
        return default
    text = text.strip()
    try:
        if text.lower().startswith("#x"):
            return int(text[2:], 16)
        if text.lower().startswith("0x"):
            return int(text, 16)
        return int(text, 10)
    except ValueError:
        return default


def _text(elem, path: str) -> str:
    found = elem.find(path)
    return found.text.strip() if found is not None and found.text else ""


class EsiDictionary:
    def __init__(self, path: str | None = None):
        self.path = path
        self.vendor_id = 0
        self.product_code = 0
        self.revision = 0
        self.device_name = ""
        self.objects: dict[int, dict] = {}
        self.subs: dict[tuple[int, int], dict] = {}
        self.tx_pdo: list[dict] = []
        self.rx_pdo: list[dict] = []
        self.error: str | None = None
        if path:
            try:
                self._load(path)
            except Exception as exc:  # ESI 仅是注释，解析失败不应阻塞主流程
                self.error = f"{type(exc).__name__}: {exc}"

    def _load(self, path: str):
        root = ET.parse(path).getroot()
        for vendor in root:
            if _local(vendor.tag) == "Vendor":
                self.vendor_id = _parse_int(_text(vendor, "Id"))
                break

        device = None
        for elem in root.iter():
            if _local(elem.tag) == "Device":
                device = elem
                break
        if device is None:
            return

        type_elem = device.find("Type")
        if type_elem is not None:
            self.product_code = _parse_int(type_elem.get("ProductCode", "0"))
            self.revision = _parse_int(type_elem.get("RevisionNo", "0"))
            self.device_name = (type_elem.text or "").strip()

        for obj in device.iter():
            if _local(obj.tag) != "Object":
                continue
            index = _parse_int(_text(obj, "Index"))
            if not index:
                continue
            entry = {
                "index": index,
                "name": _text(obj, "Name"),
                "type": _text(obj, "Type"),
                "bits": _parse_int(_text(obj, "BitSize")),
                "access": _text(obj, "Flags/Access").lower(),
                "pdo_mapping": _text(obj, "Flags/PdoMapping").lower(),
            }
            self.objects[index] = entry
            for sub in obj.iter():
                if _local(sub.tag) != "SubItem":
                    continue
                sub_idx = _parse_int(_text(sub, "SubIdx"))
                self.subs[(index, sub_idx)] = {
                    "index": index,
                    "sub": sub_idx,
                    "name": _text(sub, "Name"),
                    "type": _text(sub, "Type"),
                    "bits": _parse_int(_text(sub, "BitSize")),
                    "access": _text(sub, "Flags/Access").lower(),
                }

        for pdo in device.iter():
            tag = _local(pdo.tag)
            if tag not in ("RxPdo", "TxPdo"):
                continue
            entries = []
            bit = 0
            for ent in pdo:
                if _local(ent.tag) != "Entry":
                    continue
                index = _parse_int(_text(ent, "Index"))
                sub = _parse_int(_text(ent, "SubIndex"))
                bits = _parse_int(_text(ent, "BitLen"))
                name = _text(ent, "Name")
                entries.append({
                    "index": index, "sub": sub, "bits": bits, "bit_offset": bit,
                    "name": name or self.signal_name(index, sub),
                })
                bit += bits
            item = {"sm": _parse_int(pdo.get("Sm", "0")), "entries": entries, "bits": bit}
            if tag == "RxPdo":
                self.rx_pdo.append(item)
            else:
                self.tx_pdo.append(item)

    def signal_name(self, index: int, sub: int) -> str:
        sub_entry = self.subs.get((index, sub))
        name = sub_entry.get("name") if sub_entry else ""
        if name and not name.lower().startswith("subindex"):
            return name
        obj = self.objects.get(index)
        if obj and obj.get("name"):
            return obj["name"]
        return ""

    def describe(self, index: int, sub: int | None = None) -> dict:
        obj = self.objects.get(index, {})
        result = {
            "name": obj.get("name", ""),
            "type": obj.get("type", ""),
            "bits": obj.get("bits", 0),
            "access": obj.get("access", ""),
        }
        if sub is not None:
            item = self.subs.get((index, sub))
            if item:
                result.update({"name": item["name"] or result["name"], "type": item["type"], "bits": item["bits"], "access": item["access"]})
        return result
