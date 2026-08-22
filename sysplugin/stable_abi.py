from pathlib import Path
import hashlib
import struct

from semantic_marker import attach_dwarf_addresses, load_semantic_anchors

KIND_FUNC = 1
KIND_OBJECT = 2
KIND_NOTYPE = 3

SYMBOL_TYPE_TO_KIND = {
    "STT_FUNC": KIND_FUNC,
    "STT_OBJECT": KIND_OBJECT,
    "STT_NOTYPE": KIND_NOTYPE,
}

DOMAIN = {
    KIND_FUNC: b"F",
    KIND_OBJECT: b"O",
    KIND_NOTYPE: b"N",
}


def _field(text):
    data = text.encode("utf-8")
    return struct.pack("<I", len(data)) + data


def _finish(payload):
    return int.from_bytes(hashlib.sha256(payload).digest()[:8], "little")


def symbol_key(stype, name):
    kind = SYMBOL_TYPE_TO_KIND.get(stype)
    if kind is None:
        raise ValueError(f"unsupported symbol type {stype!r}")
    return _finish(DOMAIN[kind] + _field(name))


def build_hook_points(elf_path, module_root, manifest_dir, read_decoded_locations):
    anchors = load_semantic_anchors(manifest_dir, module_root)
    resolved = attach_dwarf_addresses(
        anchors,
        read_decoded_locations(elf_path),
        module_root,
    )
    records = {}
    identity_by_key = {}
    for item in resolved:
        key = item["key"]
        canonical = item["canonical"]
        previous = identity_by_key.get(key)
        if previous is not None and previous != canonical:
            raise SystemExit(
                f"u64 semantic hook-key collision 0x{key:016X}: "
                f"{previous!r} versus {canonical!r}"
            )
        identity_by_key[key] = canonical
        records[key] = item
    return records, len(anchors)


def add_unique_key(table, key, value, identity):
    previous = table.get(key)
    record = (value & 0xFFFFFFFF, identity)
    if previous is None:
        table[key] = record
        return
    if previous[0] != record[0] or previous[1] != identity:
        raise SystemExit(
            f"u64 key collision/duplicate 0x{key:016X}: "
            f"{previous[1]!r} -> 0x{previous[0]:08X}, "
            f"{identity!r} -> 0x{record[0]:08X}"
        )