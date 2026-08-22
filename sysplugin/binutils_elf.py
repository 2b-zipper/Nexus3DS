"""ELF access for the Nexus .3nx developer kit.

The historical filename is retained because create3nx.py imports it, but ELF
metadata and DWARF are parsed structurally with the bundled pyelftools 0.33.
Only arm-none-eabi-objcopy still comes from binutils when create3nx.py emits a
flat plugin image.
"""

from pathlib import Path
import hashlib
import os
import posixpath
import shutil
import sys


VENDOR_ROOT = Path(__file__).resolve().parent / "vendor" / "pyelftools"
if not (VENDOR_ROOT / "elftools" / "elf" / "elffile.py").is_file():
    raise SystemExit(
        f"missing bundled pyelftools 0.33 at {VENDOR_ROOT}; "
        "re-extract the complete Nexus plugin developer kit"
    )

sys.path.insert(0, str(VENDOR_ROOT))

try:
    from elftools import __version__ as PYELFTOOLS_VERSION
    from elftools.common.exceptions import ELFError
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection
    from elftools.elf.sections import SymbolTableSection
except ImportError as exc:
    raise SystemExit(f"failed to import bundled pyelftools 0.33: {exc}")

if PYELFTOOLS_VERSION != "0.33":
    raise SystemExit(
        f"unsupported bundled pyelftools version {PYELFTOOLS_VERSION!r}; expected '0.33'"
    )


def find_tool(name):
    """Find a devkitARM executable through PATH or the normal SDK roots."""
    found = shutil.which(name)
    if found:
        return found

    candidates = []

    def add_bin(bin_dir):
        bin_dir = Path(bin_dir)
        candidates.append(bin_dir / name)
        candidates.append(bin_dir / (name + ".exe"))

    devkitarm = os.environ.get("DEVKITARM")
    if devkitarm:
        add_bin(Path(devkitarm) / "bin")

    devkitpro = os.environ.get("DEVKITPRO")
    if devkitpro:
        add_bin(Path(devkitpro) / "devkitARM" / "bin")

    add_bin("/opt/devkitpro/devkitARM/bin")

    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)

    searched = ", ".join(str(candidate) for candidate in candidates)
    raise SystemExit(
        f"missing {name}; checked PATH"
        + (f" and {searched}" if searched else "")
    )


def backend_fingerprint():
    """Hash the exact bundled parser used for marker and .3nx generation."""
    digest = hashlib.sha256()
    files = [
        path for path in VENDOR_ROOT.rglob("*")
        if path.is_file() and (
            path.suffix == ".py"
            or path.name in ("py.typed", "LICENSE")
        )
    ]

    for path in sorted(files, key=lambda item: item.as_posix()):
        relative = path.relative_to(VENDOR_ROOT).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")

    return digest.hexdigest()


class Section:
    def __init__(self, index, name, stype, addr, size, link=0, info=0):
        self.index = index
        self.name = name
        self.stype = stype
        self._v = {
            "sh_addr": addr,
            "sh_size": size,
            "sh_link": link,
            "sh_info": info,
        }

    def __getitem__(self, key):
        return self._v[key]


class Symbol:
    def __init__(self, name, value, ndx, stype, size=0, binding="STB_LOCAL"):
        type_name = str(stype)
        if not type_name.startswith("STT_"):
            type_name = "STT_" + type_name

        binding_name = str(binding)
        if not binding_name.startswith("STB_"):
            binding_name = "STB_" + binding_name

        self.name = name
        self._v = {
            "st_value": value,
            "st_size": size,
            "st_shndx": ndx,
            "st_info": {"type": type_name, "bind": binding_name},
        }

    def __getitem__(self, key):
        return self._v[key]


class Relocation:
    def __init__(self, offset, rel_type, symbol):
        self._v = {
            "r_offset": offset,
            "r_info_type": rel_type,
        }
        self.symbol = symbol

    def __getitem__(self, key):
        return self._v[key]


class RelocationGroup:
    def __init__(self, name, target_name, relocs):
        self.name = name
        self.target_name = target_name
        self.relocations = relocs


def _section_type_name(value):
    name = str(value)
    return name[4:] if name.startswith("SHT_") else name


def _open_error(path, exc):
    raise SystemExit(f"failed to parse ELF {path}: {exc}")


def read_sections(path):
    path = Path(path)
    try:
        with path.open("rb") as stream:
            elf = ELFFile(stream)
            return [
                Section(
                    index,
                    section.name,
                    _section_type_name(section["sh_type"]),
                    section["sh_addr"],
                    section["sh_size"],
                    section["sh_link"],
                    section["sh_info"],
                )
                for index, section in enumerate(elf.iter_sections())
            ]
    except (OSError, ELFError, KeyError, ValueError) as exc:
        _open_error(path, exc)


def _copy_symbol(symbol):
    return Symbol(
        symbol.name,
        symbol["st_value"],
        symbol["st_shndx"],
        symbol["st_info"]["type"],
        symbol["st_size"],
        symbol["st_info"]["bind"],
    )


def read_symbols(path):
    path = Path(path)
    out = []

    try:
        with path.open("rb") as stream:
            elf = ELFFile(stream)
            for section in elf.iter_sections():
                if isinstance(section, SymbolTableSection):
                    out.extend(_copy_symbol(symbol) for symbol in section.iter_symbols())
    except (OSError, ELFError, KeyError, ValueError) as exc:
        _open_error(path, exc)

    if not out:
        raise SystemExit(f"ELF has no symbol table: {path}")
    return out


def read_relocations(path, wanted_target_names=None):
    path = Path(path)
    groups = []
    wanted_targets = (
        None if wanted_target_names is None else set(wanted_target_names)
    )

    try:
        with path.open("rb") as stream:
            elf = ELFFile(stream)

            for section in elf.iter_sections():
                if not isinstance(section, RelocationSection):
                    continue

                target = elf.get_section(section["sh_info"])
                if target is None:
                    raise ELFError(
                        f"bad target section for {section.name!r}"
                    )

                # Nexus retains relocations for the complete host image through
                # --emit-relocs. A .3nx build only consumes relocations whose
                # target is one of its plugin sections, so reject unrelated
                # groups before decoding their potentially enormous entry lists.
                if wanted_targets is not None and target.name not in wanted_targets:
                    continue

                symbol_table = elf.get_section(section["sh_link"])
                if not isinstance(symbol_table, SymbolTableSection):
                    raise ELFError(
                        f"bad linked symbol section for {section.name!r}"
                    )

                relocs = []
                symbol_cache = {}
                for relocation in section.iter_relocations():
                    symbol_index = relocation["r_info_sym"]
                    if symbol_index >= symbol_table.num_symbols():
                        raise ELFError(
                            f"bad symbol index {symbol_index} in {section.name!r}"
                        )

                    if symbol_index not in symbol_cache:
                        symbol_cache[symbol_index] = _copy_symbol(
                            symbol_table.get_symbol(symbol_index)
                        )

                    relocs.append(Relocation(
                        relocation["r_offset"],
                        relocation["r_info_type"],
                        symbol_cache[symbol_index],
                    ))

                groups.append(RelocationGroup(section.name, target.name, relocs))
    except (OSError, ELFError, KeyError, ValueError) as exc:
        _open_error(path, exc)

    return groups


def _decode(value):
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def _line_file_path(header, file_index):
    """Resolve one DWARF line-state file index for DWARF v2 through v5."""
    index_delta = 1 if header.version < 5 else 0
    index = file_index - index_delta
    file_entries = header["file_entry"]

    if index < 0 or index >= len(file_entries):
        return None

    file_entry = file_entries[index]
    name = _decode(file_entry.name)
    directory_index = file_entry["dir_index"]

    if directory_index == 0 and header.version < 5:
        return name

    directory_index -= index_delta
    directories = header["include_directory"]
    if directory_index < 0 or directory_index >= len(directories):
        return name

    directory = _decode(directories[directory_index])
    return posixpath.join(directory, name) if directory else name


def read_decoded_locations(path, wanted_basenames=None):
    """Yield (source_path, line, column, address, is_stmt) from linked DWARF."""
    path = Path(path)
    wanted = None
    if wanted_basenames is not None:
        wanted = {str(name).casefold() for name in wanted_basenames}

    try:
        with path.open("rb") as stream:
            elf = ELFFile(stream)
            if elf["e_type"] not in ("ET_EXEC", "ET_DYN"):
                raise ELFError(
                    f"DWARF marker resolution requires a linked ELF, got {elf['e_type']}"
                )
            if not elf.has_dwarf_info():
                raise ELFError("ELF has no DWARF information")

            dwarf = elf.get_dwarf_info(relocate_dwarf_sections=False)
            for compilation_unit in dwarf.iter_CUs():
                line_program = dwarf.line_program_for_CU(compilation_unit)
                if line_program is None:
                    continue

                paths = {}
                for entry in line_program.get_entries():
                    state = entry.state
                    if (
                        state is None
                        or state.end_sequence
                        or not state.address
                        or state.line is None
                    ):
                        continue

                    source_path = paths.get(state.file)
                    if state.file not in paths:
                        source_path = _line_file_path(line_program.header, state.file)
                        paths[state.file] = source_path

                    if source_path is None:
                        continue

                    basename = source_path.replace("\\", "/").rsplit("/", 1)[-1]
                    if wanted is not None and basename.casefold() not in wanted:
                        continue

                    yield (
                        source_path,
                        int(state.line),
                        int(state.column or 0),
                        int(state.address),
                        bool(state.is_stmt),
                    )
    except (OSError, ELFError, KeyError, IndexError, ValueError) as exc:
        _open_error(path, exc)


def read_decoded_lines(path, wanted_basenames=None):
    """Compatibility view yielding (source_path, line, address)."""
    for source_path, line, _column, address, _is_stmt in read_decoded_locations(path, wanted_basenames):
        yield source_path, line, address