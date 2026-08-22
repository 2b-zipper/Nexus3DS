from pathlib import Path
import argparse
import hashlib
import struct
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from binutils_elf import read_decoded_locations, read_sections, read_symbols
from stable_abi import SYMBOL_TYPE_TO_KIND, add_unique_key, build_hook_points, symbol_key

VENDOR_ROOT = HERE / "vendor" / "pyelftools"
sys.path.insert(0, str(VENDOR_ROOT))
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

MAGIC = 0x21524E33
HEADER_SIZE = 0x20
RECORD_SIZE = 0x0C


def align(value, amount):
    return (value + amount - 1) & ~(amount - 1)


def address_in_sections(value, sections):
    masked = value & ~1
    for section in sections:
        start = int(section["sh_addr"])
        size = int(section["sh_size"])
        if size and start <= masked < start + size:
            return True
    return False


def build_module_records(module_name, elf_path, module_root, manifest_dir):
    symbols = read_symbols(elf_path)
    sections = read_sections(elf_path)
    table = {}
    candidates = {}
    named = 0

    for sym in symbols:
        if not sym.name or sym["st_shndx"] == "SHN_UNDEF":
            continue
        stype = sym["st_info"]["type"]
        if stype not in SYMBOL_TYPE_TO_KIND:
            continue
        value = int(sym["st_value"])
        if not address_in_sections(value, sections):
            continue
        candidates.setdefault((stype, sym.name), set()).add((value, int(sym["st_size"])))

    for (stype, name), locations in candidates.items():
        if len(locations) != 1:
            continue
        value, size = next(iter(locations))
        add_unique_key(table, symbol_key(stype, name), value, ("symbol", stype, name, size if stype == "STT_OBJECT" else 0))
        named += 1

    hooks, semantic_total = build_hook_points(
        elf_path, module_root, manifest_dir, read_decoded_locations
    )
    for record in hooks.values():
        add_unique_key(table, record["key"], record["address"], ("hook",) + record["canonical"])

    records = sorted((key, value) for key, (value, _) in table.items())
    print(
        f"{module_name}: {named} named symbol(s), "
        f"{len(hooks)}/{semantic_total} semantic hook point(s), {len(records)} record(s)"
    )
    return records


def pack_records(records):
    return b"".join(struct.pack("<QI", key, value) for key, value in records)


def elf_symbol_values(elf_path):
    values = {}
    for symbol in read_symbols(elf_path):
        if symbol.name:
            values.setdefault(symbol.name, int(symbol["st_value"]))
    return values


def extract_fixer(elf_path):
    elf_path = Path(elf_path)
    symbols = elf_symbol_values(elf_path)
    required = ["Main", "__fixer_code_end", "__fixer_data_start", "__fixer_data_end"]
    for name in required:
        if name not in symbols:
            raise SystemExit(f"fixer ELF missing {name}")

    if symbols["Main"] != 0:
        raise SystemExit(f"fixer Main is 0x{symbols['Main']:X}, expected 0")

    code_size = symbols["__fixer_code_end"]
    data_start = symbols["__fixer_data_start"]
    data_end = symbols["__fixer_data_end"]
    if not code_size or data_start != align(code_size, 0x1000) or data_end < data_start:
        raise SystemExit("invalid fixer code/data linker layout")

    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        relocations = [section.name for section in elf.iter_sections() if isinstance(section, RelocationSection) and section.num_relocations()]
        if relocations:
            raise SystemExit("fixer ELF still contains relocation sections: " + ", ".join(relocations))

        text = elf.get_section_by_name(".text")
        if text is None or int(text["sh_addr"]) != 0 or int(text["sh_size"]) != code_size:
            raise SystemExit("fixer .text does not exactly match the linked code span")
        code = text.data()

        data_size = data_end - data_start
        initialized = elf.get_section_by_name(".data")
        if initialized is not None and int(initialized["sh_size"]):
            raise SystemExit("fixer must not contain initialized .data; use const/.rodata or initialize writable state in Main")

    return bytes(code), data_size


def write_if_changed(path, data):
    path = Path(path)
    if path.exists() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(data)
    temp.replace(path)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixer-elf", required=True)
    parser.add_argument("--loader-elf", required=True)
    parser.add_argument("--loader-root", required=True)
    parser.add_argument("--loader-manifests", required=True)
    parser.add_argument("--rosalina-elf", required=True)
    parser.add_argument("--rosalina-root", required=True)
    parser.add_argument("--rosalina-manifests", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--k11-header", required=True)
    args = parser.parse_args()

    code, data_size = extract_fixer(args.fixer_elf)
    loader_records = build_module_records(
        "loader", Path(args.loader_elf), Path(args.loader_root), Path(args.loader_manifests)
    )
    rosalina_records = build_module_records(
        "rosalina", Path(args.rosalina_elf), Path(args.rosalina_root), Path(args.rosalina_manifests)
    )
    loader_blob = pack_records(loader_records)
    rosalina_blob = pack_records(rosalina_records)

    host_identity = struct.pack("<II", len(loader_records), len(rosalina_records)) + loader_blob + rosalina_blob

    code_offset = HEADER_SIZE
    table_offset = code_offset + len(code)
    file_size = table_offset + len(loader_blob) + len(rosalina_blob)

    pair_payload = (
        b"NEXUS_BOOT_3NR_PAIR\0"
        + struct.pack("<IIIII", code_offset, len(code), data_size, len(loader_records), len(rosalina_records))
        + code
        + host_identity
    )
    build_pair_id = int.from_bytes(hashlib.sha256(pair_payload).digest()[:8], "little") & 0xFFFFFFFFFFFFFFFE

    header = struct.pack(
        "<IQIIIII",
        MAGIC,
        build_pair_id,
        code_offset,
        len(code),
        data_size,
        len(loader_records),
        len(rosalina_records),
    )
    if len(header) != HEADER_SIZE:
        raise SystemExit("internal 3NR header-size mismatch")

    output = header + code + loader_blob + rosalina_blob
    if len(output) != file_size:
        raise SystemExit("internal 3NR file-size mismatch")

    pair_lo = build_pair_id & 0xFFFFFFFF
    pair_hi = build_pair_id >> 32
    generated = (
        "#pragma once\n\n"
        f"#define SYSPLUGIN_3NR_BUILD_PAIR_LO 0x{pair_lo:08X}u\n"
        f"#define SYSPLUGIN_3NR_BUILD_PAIR_HI 0x{pair_hi:08X}u\n"
        f"#define SYSPLUGIN_3NR_KEYED_PATH \"/boot.{pair_lo:08X}.3nr\"\n"
        f"#define SYSPLUGIN_3NR_CODE_OFFSET 0x{code_offset:X}u\n"
        f"#define SYSPLUGIN_3NR_CODE_SIZE 0x{len(code):X}u\n"
        f"#define SYSPLUGIN_3NR_DATA_SIZE 0x{data_size:X}u"
    ).encode("ascii")

    changed = write_if_changed(args.output, output)
    if not changed:
        Path(args.output).touch()
    header_changed = write_if_changed(args.k11_header, generated)

    print(f"3NR fixer: Main +0x0, code 0x{len(code):X}, BSS 0x{data_size:X}, file code +0x{code_offset:X}")
    print(f"build pair ID: 0x{build_pair_id:016X}")
    print(f"boot.3nr size: {len(output)} bytes")
    print(("wrote " if changed else "unchanged ") + args.output)
    print(("wrote " if header_changed else "unchanged ") + args.k11_header)


if __name__ == "__main__":
    main()