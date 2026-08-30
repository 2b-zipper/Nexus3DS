from pathlib import Path
import os
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
K11_ELF = ROOT / "k11_extension" / "k11_extension.elf"
K11_PAIR_HEADER = ROOT / "k11_extension" / "include" / "svc" / "SysPlugin3nrGenerated.h"
K11_ENTRY_HEADER = HERE / "include" / "SysPluginLoaderEntryGenerated.h"

K11_VA = 0x70000000
O3DS_SYSTEM_TOP = 0x26C00000
N3DS_SYSTEM_TOP = 0x2E000000
MAX_PASSES = 8

from binutils_elf import read_symbols


def write_if_changed(path, data):
    path = Path(path)
    if path.exists() and path.read_bytes() == data:
        return False

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)
    return True


def run_make(directory, target="all"):
    command = shlex.split(os.environ.get("MAKE", "make"))
    command += ["--no-print-directory", "-C", str(directory), target]
    try:
        subprocess.run(command, check=True, close_fds=False)
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"make failed in {directory} with status {exc.returncode}") from exc


def write_bootstrap_pair_header():
    data = (
        "#pragma once\n\n"
        "#define SYSPLUGIN_3NR_BUILD_PAIR_LO 0x00000000u\n"
        "#define SYSPLUGIN_3NR_BUILD_PAIR_HI 0x00000000u\n"
        "#define SYSPLUGIN_3NR_KEYED_PATH \"/boot.00000000.3nr\"\n"
        "#define SYSPLUGIN_3NR_CODE_OFFSET 0x20u\n"
        "#define SYSPLUGIN_3NR_CODE_SIZE 0x0u\n"
        "#define SYSPLUGIN_3NR_DATA_SIZE 0x0u\n"
    ).encode("ascii")
    write_if_changed(K11_PAIR_HEADER, data)


def symbol_values(elf_path):
    wanted = {"__start__", "__end__", "SysPluginLoader_Main"}
    values = {}

    for symbol in read_symbols(elf_path):
        if symbol.name not in wanted or symbol["st_shndx"] == "SHN_UNDEF":
            continue

        value = int(symbol["st_value"])
        old_value = values.get(symbol.name)
        if old_value is not None and old_value != value:
            raise SystemExit(
                f"K11 ELF has conflicting values for {symbol.name}: "
                f"0x{old_value:X} and 0x{value:X}"
            )
        values[symbol.name] = value

    missing = sorted(wanted - values.keys())
    if missing:
        raise SystemExit("K11 ELF is missing: " + ", ".join(missing))
    return values


def generate_entry_header():
    values = symbol_values(K11_ELF)
    start = values["__start__"]
    end = values["__end__"]
    entry = values["SysPluginLoader_Main"]

    if start != K11_VA:
        raise SystemExit(f"K11 __start__ is 0x{start:X}, expected 0x{K11_VA:X}")
    if end <= start or (end - start) & 0xFFF:
        raise SystemExit("K11 image size is zero or not 4 KiB aligned")
    if not start <= entry < end or entry & 3:
        raise SystemExit("SysPluginLoader_Main is outside K11 or not ARM aligned")

    image_size = end - start
    entry_offset = entry - start

    def direct_alias(system_top):
        if image_size > system_top:
            raise SystemExit("K11 image is too large for the system memory region")
        physical = system_top - image_size + entry_offset
        if physical >= 0x30000000:
            raise SystemExit("K11 entry is outside the direct physical mapping")
        return physical | 0x80000000

    o3ds_entry = direct_alias(O3DS_SYSTEM_TOP)
    n3ds_entry = direct_alias(N3DS_SYSTEM_TOP)
    data = (
        "/* Generated from k11_extension.elf by sysplugin/build_pair.py. */\n"
        "#pragma once\n\n"
        f"#define SYSPLUGIN_LOADER_ENTRY_O3DS 0x{o3ds_entry:08X}u\n"
        f"#define SYSPLUGIN_LOADER_ENTRY_N3DS 0x{n3ds_entry:08X}u\n"
    ).encode("ascii")
    changed = write_if_changed(K11_ENTRY_HEADER, data)
    print(
        f"K11 sysplugin entry: VA 0x{entry:08X}, size 0x{image_size:X}, "
        f"O3DS 0x{o3ds_entry:08X}, N3DS 0x{n3ds_entry:08X}"
    )
    return changed


def bootstrap_entry_header():
    if K11_ENTRY_HEADER.exists():
        return

    made_bootstrap_pair_header = False
    if not K11_ELF.exists():
        if not K11_PAIR_HEADER.exists():
            write_bootstrap_pair_header()
            made_bootstrap_pair_header = True
        run_make(ROOT / "k11_extension")

    generate_entry_header()

    if made_bootstrap_pair_header:
        K11_PAIR_HEADER.unlink()


def main():
    bootstrap_entry_header()

    for pass_number in range(1, MAX_PASSES + 1):
        run_make(ROOT / "sysmodules")
        run_make(HERE)
        run_make(ROOT / "k11_extension")

        if not generate_entry_header():
            print(f"sysplugin pair converged after pass {pass_number}")
            return

        print(f"K11 entry changed on pass {pass_number}; rebuilding both consumers")

    raise SystemExit(f"sysplugin pair did not converge after {MAX_PASSES} passes")


if __name__ == "__main__":
    main()
