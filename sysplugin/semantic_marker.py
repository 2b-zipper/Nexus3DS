from pathlib import Path
import hashlib
import re
import struct

DOMAIN_HOOK = b"H3"
SOURCE_EXTS = {".c", ".cpp", ".cc"}
MARKER_RE = re.compile(
    r"/\*\s*PLG_MARKER\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\*/"
    r"|//\s*PLG_MARKER\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)"
)


def _field(text):
    data = text.encode("utf-8")
    return struct.pack("<I", len(data)) + data


def hook_key(relative_path, function_name, kind, semantic, occurrence):
    if occurrence < 0 or occurrence > 0xFFFFFFFF:
        raise ValueError("hook occurrence outside u32")
    payload = (
        DOMAIN_HOOK
        + _field(relative_path)
        + _field(function_name)
        + _field(kind)
        + _field(semantic)
        + struct.pack("<I", occurrence)
    )
    return int.from_bytes(hashlib.sha256(payload).digest()[:8], "little")


def normalize_relative_path(path):
    text = str(path).replace("\\", "/")
    while text.startswith("./"):
        text = text[2:]
    return text


def _source_index(module_root):
    module_root = Path(module_root).resolve()
    files = []
    for path in module_root.rglob("*"):
        if path.is_file() and path.suffix in SOURCE_EXTS:
            rel = normalize_relative_path(path.relative_to(module_root))
            files.append((path.resolve(), rel))
    by_rel = {rel.casefold(): (path, rel) for path, rel in files}
    by_abs = {normalize_relative_path(path).casefold(): (path, rel) for path, rel in files}
    by_base = {}
    for path, rel in files:
        by_base.setdefault(Path(rel).name.casefold(), []).append((path, rel))
    return files, by_rel, by_abs, by_base


def _resolve_source_path(raw_path, source_index):
    files, by_rel, by_abs, by_base = source_index
    raw = normalize_relative_path(raw_path)
    raw_fold = raw.casefold()
    if Path(raw_path).is_absolute():
        return by_abs.get(raw_fold)
    exact = by_rel.get(raw_fold)
    if exact is not None:
        return exact
    matches = []
    for path, rel in files:
        rel_fold = rel.casefold()
        if raw_fold == rel_fold or raw_fold.endswith("/" + rel_fold):
            matches.append((len(rel), path, rel))
    if matches:
        matches.sort(reverse=True, key=lambda item: item[0])
        best_len = matches[0][0]
        best = [item for item in matches if item[0] == best_len]
        if len(best) == 1:
            return best[0][1], best[0][2]
    base_matches = by_base.get(Path(raw).name.casefold(), [])
    if len(base_matches) == 1:
        return base_matches[0]
    return None


def load_semantic_anchors(manifest_dir, module_root):
    manifest_dir = Path(manifest_dir)
    module_root = Path(module_root).resolve()
    source_index = _source_index(module_root)
    if not manifest_dir.is_dir():
        raise SystemExit(f"missing semantic marker manifest directory: {manifest_dir}")

    expected_sources = {
        rel.casefold(): rel
        for _path, rel in source_index[0]
        if rel.casefold().startswith("source/")
    }
    anchors = []
    seen_inputs = {}
    manifest_versions = set()
    for path in sorted(manifest_dir.glob("*.tsv"), key=lambda p: p.name):
        lines = path.read_text(encoding="utf-8", errors="strict").splitlines()
        if not lines:
            raise SystemExit(f"invalid semantic marker manifest: {path}")
        meta = lines[0].split("\t", 3)
        if len(meta) != 4 or meta[0] != "META" or meta[1] != "SM3":
            raise SystemExit(f"invalid semantic marker manifest: {path}")
        manifest_versions.add(meta[2])
        input_resolved = _resolve_source_path(meta[3], source_index)
        if input_resolved is None:
            continue
        _input_path, input_rel = input_resolved
        input_key = input_rel.casefold()
        if input_key not in expected_sources:
            continue
        previous_manifest = seen_inputs.get(input_key)
        if previous_manifest is not None:
            raise SystemExit(
                f"duplicate semantic marker manifests for {input_rel}: "
                f"{previous_manifest} and {path}"
            )
        seen_inputs[input_key] = path

        for raw in lines[1:]:
            if not raw:
                continue
            parts = raw.split("\t", 8)
            if len(parts) != 9 or parts[0] != "ANCHOR":
                raise SystemExit(f"invalid semantic marker record in {path}: {raw}")
            _, src, function, line, col, end_line, end_col, kind, semantic = parts
            resolved = _resolve_source_path(src, source_index)
            if resolved is None:
                continue
            src_path, rel = resolved
            anchors.append({
                "src": src_path,
                "rel": rel,
                "function": function,
                "line": int(line),
                "col": int(col),
                "end_line": int(end_line),
                "end_col": int(end_col),
                "kind": kind,
                "semantic": semantic,
            })

    if len(manifest_versions) > 1:
        raise SystemExit(
            "semantic marker manifests were produced by mixed GCC versions: "
            + ", ".join(sorted(manifest_versions))
        )

    missing = [expected_sources[key] for key in sorted(expected_sources) if key not in seen_inputs]
    if missing:
        preview = ", ".join(missing[:8])
        if len(missing) > 8:
            preview += f", ... (+{len(missing) - 8} more)"
        raise SystemExit(
            f"semantic marker manifests are incomplete for {module_root}: missing {preview}"
        )

    anchors.sort(key=lambda a: (
        a["rel"].casefold(), a["function"], a["line"], a["col"],
        a["end_line"], a["end_col"], a["kind"], a["semantic"]
    ))
    counts = {}
    identity_by_key = {}
    for a in anchors:
        ident = (a["rel"], a["function"], a["kind"], a["semantic"])
        occurrence = counts.get(ident, 0)
        counts[ident] = occurrence + 1
        key = hook_key(*ident, occurrence)
        canonical = ident + (occurrence,)
        old = identity_by_key.get(key)
        if old is not None and old != canonical:
            raise SystemExit(f"u64 semantic hook-key collision 0x{key:016X}: {old!r} versus {canonical!r}")
        identity_by_key[key] = canonical
        a["occurrence"] = occurrence
        a["key"] = key
        a["canonical"] = canonical
    return anchors


def _line_col_at(text, offset):
    line = text.count("\n", 0, offset) + 1
    last = text.rfind("\n", 0, offset)
    col = offset + 1 if last < 0 else offset - last
    return line, col


def scan_markers(module_root):
    module_root = Path(module_root).resolve()
    for path in module_root.rglob("*"):
        if path.is_file() and path.suffix in {".s", ".S"}:
            text = path.read_text(encoding="utf-8", errors="replace")
            if "PLG_MARKER" in text:
                raise SystemExit(
                    f"semantic PLG_MARKER is supported only in C/C++ source: {path}"
                )
    out = {}
    for path in sorted(module_root.rglob("*"), key=lambda p: p.as_posix().casefold()):
        if not path.is_file() or path.suffix not in SOURCE_EXTS:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "PLG_MARKER" not in text:
            continue
        rel = normalize_relative_path(path.relative_to(module_root))
        for match in MARKER_RE.finditer(text):
            name = match.group(1) or match.group(2)
            if name in out:
                old = out[name]
                raise SystemExit(
                    f"duplicate marker {name!r}: {old['rel']}:{old['line']} and {rel}:{_line_col_at(text, match.start())[0]}"
                )
            line, col = _line_col_at(text, match.start())
            out[name] = {"name": name, "path": path.resolve(), "rel": rel, "line": line, "col": col, "offset": match.start()}
    return out


def _mask_noncode(text):
    out = list(text)
    i = 0
    state = "code"
    quote = None
    escape = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "line_comment":
            if c == "\n":
                state = "code"
            else:
                out[i] = " "
            i += 1
            continue
        if state == "block_comment":
            if c == "*" and n == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "code"
            else:
                if c != "\n":
                    out[i] = " "
                i += 1
            continue
        if state == "string":
            if c != "\n":
                out[i] = " "
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == quote:
                state = "code"
                quote = None
            i += 1
            continue
        if c == "/" and n == "/":
            out[i] = out[i + 1] = " "
            i += 2
            state = "line_comment"
            continue
        if c == "/" and n == "*":
            out[i] = out[i + 1] = " "
            i += 2
            state = "block_comment"
            continue
        if c in {"'", '"'}:
            out[i] = " "
            quote = c
            escape = False
            state = "string"
        i += 1
    return "".join(out)


def _line_starts(text):
    starts = [0]
    for match in re.finditer("\n", text):
        starts.append(match.end())
    return starts


def _source_offset(starts, line, col, text_len):
    if line <= 0 or line > len(starts):
        return None
    offset = starts[line - 1] + max(0, col - 1)
    return min(offset, text_len)


def _marker_terminal(masked, marker, starts):
    line_start = starts[marker["line"] - 1]
    prefix = masked[line_start:marker["offset"]].rstrip()
    if not prefix or re.search(r"\belse$", prefix):
        return None
    last = prefix[-1]
    if last == "{":
        before = prefix[:-1].rstrip()
        if not before or re.search(r"\belse$", before):
            return None
        if before[-1] == ")":
            return "control"
        return None
    if last == ")":
        return "control"
    if last == ":":
        return "label"
    if last == ";":
        return "statement"
    return None


def _anchor_terminal(anchor):
    if anchor["kind"] in {"if", "while", "for", "range_for", "switch"}:
        return "control"
    if anchor["kind"] in {"case_label_expr", "label_expr"}:
        return "label"
    return "statement"


def _top_level_semicolon_count(masked, start, end):
    paren = bracket = brace = 0
    count = 0
    for c in masked[start:end]:
        if c == "(":
            paren += 1
        elif c == ")":
            paren = max(0, paren - 1)
        elif c == "[":
            bracket += 1
        elif c == "]":
            bracket = max(0, bracket - 1)
        elif c == "{":
            brace += 1
        elif c == "}":
            brace = max(0, brace - 1)
        elif c == ";" and paren == 0 and bracket == 0 and brace == 0:
            count += 1
    return count


def resolve_source_markers(module_root, anchors):
    markers = scan_markers(module_root)
    by_rel = {}
    for anchor in anchors:
        by_rel.setdefault(anchor["rel"].casefold(), []).append(anchor)

    source_cache = {}
    resolved = {}
    for name, marker in markers.items():
        cached = source_cache.get(marker["path"])
        if cached is None:
            text = marker["path"].read_text(encoding="utf-8", errors="replace")
            cached = (text, _mask_noncode(text), _line_starts(text))
            source_cache[marker["path"]] = cached
        text, masked, starts = cached
        terminal = _marker_terminal(masked, marker, starts)

        candidates = by_rel.get(marker["rel"].casefold(), [])
        mpos = (marker["line"], marker["col"])
        preceding = [
            a for a in candidates
            if a["end_line"] == marker["line"]
            and (a["end_line"], a["end_col"]) <= mpos
        ]
        preceding.sort(key=lambda a: (
            a["end_line"], a["end_col"], a["line"], a["col"]
        ), reverse=True)

        best = preceding[0] if preceding else None
        if best is not None and _anchor_terminal(best) != terminal:
            # GCC locates the declaration in an init-only for-loop later than the
            # FOR_STMT itself. A marker after the complete header still belongs
            # to the for-loop, not to that initializer declaration.
            if terminal == "control":
                controls = [a for a in preceding if _anchor_terminal(a) == "control"]
                control = controls[0] if controls else None
                if control is not None and control["kind"] == "for":
                    later = [
                        a for a in preceding
                        if (a["end_line"], a["end_col"], a["line"], a["col"])
                        > (control["end_line"], control["end_col"], control["line"], control["col"])
                    ]
                    if all(a["kind"] in {"decl_expr", "cleanup_point_expr"} for a in later):
                        best = control
            if best is not None and _anchor_terminal(best) != terminal:
                best = None

        if best is not None and terminal == "statement":
            start = _source_offset(starts, best["line"], best["col"], len(text))
            if start is None or _top_level_semicolon_count(masked, start, marker["offset"]) != 1:
                best = None

        if best is None or terminal is None:
            raise SystemExit(
                f"marker {name!r} at {marker['rel']}:{marker['line']}:{marker['col']} "
                "is not attached to a GCC semantic construct at that source position"
            )
        resolved[name] = {**marker, "anchor": best}
    return resolved

def attach_dwarf_addresses(anchors, decoded_locations, module_root):
    source_index = _source_index(module_root)
    anchors_by_line = {}
    for anchor in anchors:
        anchors_by_line.setdefault((anchor["rel"].casefold(), anchor["line"]), []).append(anchor)

    by_pos = {}
    by_line = {}
    for raw_path, line, column, address, is_stmt in decoded_locations:
        if line is None or int(line) <= 0 or not address:
            continue
        resolved_source = _resolve_source_path(raw_path, source_index)
        if resolved_source is None:
            continue
        src_path, rel = resolved_source
        line = int(line)
        column = int(column or 0)
        if (rel.casefold(), line) not in anchors_by_line:
            continue
        row = (int(address), bool(is_stmt))
        by_pos.setdefault((src_path, line, column), []).append(row)
        by_line.setdefault((src_path, line), []).append((column, row[0], row[1]))

    directly_resolved = []
    for anchor in anchors:
        rows = by_pos.get((anchor["src"], anchor["line"], anchor["col"]))
        if rows:
            rows.sort(key=lambda row: (0 if row[1] else 1, row[0]))
            anchor["address"] = rows[0][0]
            anchor["dwarf_column"] = anchor["col"]
            anchor["dwarf_exact"] = True
            anchor["dwarf_forward"] = False
            directly_resolved.append(anchor)
            continue

        line_anchors = anchors_by_line.get((anchor["rel"].casefold(), anchor["line"]), [])
        nearby = []
        for col, addr, is_stmt in by_line.get((anchor["src"], anchor["line"]), []):
            def source_distance(candidate):
                if col < candidate["col"]:
                    return candidate["col"] - col
                if candidate["end_line"] == candidate["line"] and col > candidate["end_col"]:
                    return col - candidate["end_col"]
                return 0
            distances = [(source_distance(candidate), candidate) for candidate in line_anchors]
            best_distance = min((distance for distance, _candidate in distances), default=None)
            winners = [candidate for distance, candidate in distances if distance == best_distance]
            if len(winners) == 1 and winners[0] is anchor:
                nearby.append((best_distance, 0 if is_stmt else 1, addr, col))
        if nearby:
            nearby.sort()
            _, _, addr, col = nearby[0]
            anchor["address"] = addr
            anchor["dwarf_column"] = col
            anchor["dwarf_exact"] = False
            anchor["dwarf_forward"] = False
            directly_resolved.append(anchor)

    # Some frontend constructs have a stable semantic identity but no independent
    # machine-code location after lowering/optimisation (for example, a local
    # declaration whose value is folded into the following statement). Preserve
    # that construct's key, but map it to the first later semantic construct in
    # the same lexical function that does have a real linked DWARF address.
    # Never cross a function boundary or manufacture an epilogue/prologue site.
    by_function = {}
    for anchor in anchors:
        by_function.setdefault((anchor["rel"].casefold(), anchor["function"]), []).append(anchor)

    for function_anchors in by_function.values():
        function_anchors.sort(key=lambda a: (
            a["line"], a["col"], a["end_line"], a["end_col"],
            a["kind"], a["semantic"], a["occurrence"]
        ))
        for index, anchor in enumerate(function_anchors):
            if "address" in anchor:
                continue
            end_pos = (anchor["end_line"], anchor["end_col"])
            for candidate in function_anchors[index + 1:]:
                if (candidate["line"], candidate["col"]) < end_pos:
                    continue
                if "address" not in candidate:
                    continue
                anchor["address"] = candidate["address"]
                anchor["dwarf_column"] = candidate.get("dwarf_column", candidate["col"])
                anchor["dwarf_exact"] = False
                anchor["dwarf_forward"] = True
                anchor["dwarf_forward_line"] = candidate["line"]
                anchor["dwarf_forward_column"] = candidate["col"]
                anchor["dwarf_forward_kind"] = candidate["kind"]
                break

    return [anchor for anchor in anchors if "address" in anchor]


def marker_fingerprint(module_root, extra_paths=()):
    module_root = Path(module_root).resolve()
    markers = scan_markers(module_root)
    h = hashlib.sha256()
    h.update(b"NEXUS_SEMANTIC_MARKERS_SM3\0")
    for path in extra_paths:
        path = Path(path)
        h.update(path.name.encode("utf-8"))
        h.update(b"\0")
        h.update(path.read_bytes())
        h.update(b"\0")
    marked_files = sorted({rec["path"] for rec in markers.values()}, key=lambda p: p.as_posix().casefold())
    for path in marked_files:
        rel = normalize_relative_path(path.relative_to(module_root))
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        h.update(path.read_bytes())
        h.update(b"\0")
    return h.hexdigest()
