#!/usr/bin/env python3
"""
Generate the auto-reference block in docs/Scripting.md from source-of-truth APIs:
- src/ScriptRuntime.h           (C++ ScriptContext)
- include/ScriptRuntimeCAPI.h   (C API)
- Scripts/Managed/ModuCPP.cs    (C# managed API)

Doc note convention (optional, per member):
- /// plain summary text
- // @doc: plain summary text
- /// @summary ...
- /// @usage ...
- /// @howto ...
- /// @param name ...
- /// @returns ...
- /// @note ...
- /// @example
- ///   ctx.SetPosition(...)
- /// @endexample
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, List, Optional, Tuple


MARKER_START = "<!-- AUTO-GEN:SCRIPTING_API:START -->"
MARKER_END = "<!-- AUTO-GEN:SCRIPTING_API:END -->"


@dataclass
class DocInfo:
    summary: str = ""
    usage: List[str] = field(default_factory=list)
    howto: List[str] = field(default_factory=list)
    params: List[Tuple[str, str]] = field(default_factory=list)
    returns: str = ""
    notes: List[str] = field(default_factory=list)
    example: List[str] = field(default_factory=list)

    def has_content(self) -> bool:
        return bool(
            self.summary
            or self.usage
            or self.howto
            or self.params
            or self.returns
            or self.notes
            or self.example
        )


@dataclass
class ApiItem:
    group: str
    signature: str
    doc: DocInfo


def normalize_ws(text: str) -> str:
    return " ".join(text.replace("\t", " ").split()).strip()


def _extract_raw_doc_lines(lines: List[str], start_idx: int) -> List[str]:
    docs: List[str] = []
    i = start_idx - 1
    while i >= 0:
        raw = lines[i]
        s = raw.strip()
        if not s:
            if docs:
                break
            i -= 1
            continue
        if s.startswith("///"):
            docs.append(s[3:].strip())
            i -= 1
            continue
        if s.startswith("// @doc:"):
            docs.append(s[len("// @doc:") :].strip())
            i -= 1
            continue
        if s.startswith("//@doc:"):
            docs.append(s[len("//@doc:") :].strip())
            i -= 1
            continue
        if s.startswith("// @doc "):
            docs.append(s[len("// @doc ") :].strip())
            i -= 1
            continue
        if s.startswith("//@doc "):
            docs.append(s[len("//@doc ") :].strip())
            i -= 1
            continue
        break
    docs.reverse()
    return docs


def _parse_plain_prefixed_line(line: str) -> Optional[Tuple[str, str]]:
    """Support non-tag style lines like 'Usage: ...', 'How to use: ...', etc."""
    m = re.match(r"^(summary|usage|how\s*to\s*use|howto|returns|note)\s*:\s*(.+)$", line, re.IGNORECASE)
    if not m:
        return None
    key = m.group(1).lower().replace(" ", "")
    value = m.group(2).strip()
    return key, value


def extract_inline_doc(lines: List[str], start_idx: int) -> DocInfo:
    raw_docs = _extract_raw_doc_lines(lines, start_idx)
    info = DocInfo()

    summary_parts: List[str] = []
    in_example = False

    def append_once(target: List[str], text: str) -> None:
        cleaned = normalize_ws(text)
        if cleaned:
            target.append(cleaned)

    for raw_line in raw_docs:
        line = raw_line.strip()
        if not line:
            continue

        if in_example:
            lower = line.lower()
            if lower in ("@endexample", "@example.end"):
                in_example = False
                continue
            info.example.append(raw_line.rstrip())
            continue

        lower = line.lower()

        if lower in ("@example", "@example:"):
            in_example = True
            continue
        if lower.startswith("@example "):
            append_once(info.example, line[len("@example ") :])
            continue

        if lower.startswith("@summary "):
            summary_parts.append(line[len("@summary ") :])
            continue
        if lower.startswith("@usage "):
            append_once(info.usage, line[len("@usage ") :])
            continue
        if lower.startswith("@howto "):
            append_once(info.howto, line[len("@howto ") :])
            continue
        if lower.startswith("@returns "):
            info.returns = normalize_ws(line[len("@returns ") :])
            continue
        if lower.startswith("@note "):
            append_once(info.notes, line[len("@note ") :])
            continue
        if lower.startswith("@param "):
            tail = line[len("@param ") :].strip()
            if tail:
                parts = tail.split(None, 1)
                name = parts[0]
                desc = normalize_ws(parts[1]) if len(parts) > 1 else ""
                info.params.append((name, desc))
            continue

        prefixed = _parse_plain_prefixed_line(line)
        if prefixed is not None:
            key, value = prefixed
            if key == "summary":
                summary_parts.append(value)
            elif key == "usage":
                append_once(info.usage, value)
            elif key in ("howto", "howtouse"):
                append_once(info.howto, value)
            elif key == "returns":
                info.returns = normalize_ws(value)
            elif key == "note":
                append_once(info.notes, value)
            continue

        # Backward-compatible plain docs become summary text.
        summary_parts.append(line)

    info.summary = normalize_ws(" ".join(summary_parts))
    return info


def group_items(items: Iterable[ApiItem]) -> OrderedDict[str, List[ApiItem]]:
    grouped: OrderedDict[str, List[ApiItem]] = OrderedDict()
    for item in items:
        grouped.setdefault(item.group, []).append(item)
    return grouped


def render_grouped_items(items: Iterable[ApiItem]) -> List[str]:
    out: List[str] = []
    grouped = group_items(items)
    for group, entries in grouped.items():
        out.append(f"#### {group}")
        for entry in entries:
            if not entry.doc.has_content():
                out.append(f"- `{entry.signature}`")
                continue

            out.append(f"- `{entry.signature}`")
            if entry.doc.summary:
                out.append(f"  Summary: {entry.doc.summary}")
            for usage in entry.doc.usage:
                out.append(f"  Usage: {usage}")
            for howto in entry.doc.howto:
                out.append(f"  How to use: {howto}")
            if entry.doc.params:
                params_text = "; ".join(
                    f"`{name}`: {desc}" if desc else f"`{name}`"
                    for name, desc in entry.doc.params
                )
                out.append(f"  Parameters: {params_text}")
            if entry.doc.returns:
                out.append(f"  Returns: {entry.doc.returns}")
            for note in entry.doc.notes:
                out.append(f"  Note: {note}")
            if entry.doc.example:
                example_text = " ".join(normalize_ws(x) for x in entry.doc.example if normalize_ws(x))
                if example_text:
                    out.append(f"  Example: {example_text}")
        out.append("")
    return out


def parse_script_context(path: Path) -> Tuple[List[ApiItem], List[ApiItem]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    start = None
    for i, line in enumerate(lines):
        if "struct ScriptContext" in line and "{" in line:
            start = i
            break
    if start is None:
        raise RuntimeError("Unable to find 'struct ScriptContext' in ScriptRuntime.h")

    fields: List[ApiItem] = []
    methods: List[ApiItem] = []

    depth = lines[start].count("{") - lines[start].count("}")
    current_group = "General"

    pending: List[str] = []
    pending_start = -1

    for idx in range(start + 1, len(lines)):
        line = lines[idx]
        stripped = line.strip()
        current_depth = depth

        if current_depth <= 0:
            break

        # Drop any partial statement when entering nested scopes.
        if current_depth != 1 and pending:
            pending = []

        if current_depth == 1 and stripped.startswith("//"):
            # Track section headings from existing ScriptContext comments.
            if not (
                stripped.startswith("///")
                or stripped.startswith("// @doc")
                or stripped.startswith("//@doc")
            ):
                heading = stripped[2:].strip()
                if heading:
                    current_group = heading
            depth += line.count("{") - line.count("}")
            continue

        if current_depth == 1 and not stripped.startswith("#"):
            # Skip nested type openers at top-level inside ScriptContext.
            if "{" in stripped:
                depth += line.count("{") - line.count("}")
                continue

            if not pending:
                if not stripped:
                    depth += line.count("{") - line.count("}")
                    continue
                pending_start = idx
            pending.append(stripped)

            if ";" in stripped:
                statement = normalize_ws(" ".join(pending))
                pending = []

                if statement in (";", "};", "} ;"):
                    depth += line.count("{") - line.count("}")
                    continue

                doc = extract_inline_doc(lines, pending_start)

                # Method declaration (function prototypes in ScriptContext).
                if (
                    "(" in statement
                    and ")" in statement
                    and not statement.startswith("using ")
                    and not statement.startswith("typedef ")
                ):
                    signature = statement.rstrip(";")
                    methods.append(ApiItem(current_group, signature, doc))
                else:
                    # Top-level fields/types within ScriptContext.
                    if not statement.startswith(("struct ", "class ", "enum ")):
                        signature = statement.rstrip(";")
                        fields.append(ApiItem(current_group, signature, doc))

        depth += line.count("{") - line.count("}")

    # Remove obvious artifacts if they leaked in.
    fields = [f for f in fields if f.signature and f.signature != "}"]
    methods = [m for m in methods if re.search(r"\b[A-Za-z_]\w*\s*\(", m.signature)]
    return fields, methods


def parse_c_api(path: Path) -> List[ApiItem]:
    lines = path.read_text(encoding="utf-8").splitlines()
    items: List[ApiItem] = []
    group = "C API Functions"

    pending: List[str] = []
    pending_start = -1

    for idx, line in enumerate(lines):
        stripped = line.strip()

        if stripped.startswith("//"):
            if not (
                stripped.startswith("///")
                or stripped.startswith("// @doc")
                or stripped.startswith("//@doc")
            ):
                heading = stripped[2:].strip()
                if heading and heading.lower() not in ("cplusplus",):
                    group = heading
            continue

        if not pending and not stripped:
            continue
        if stripped.startswith("#"):
            continue

        if not pending:
            pending_start = idx
        pending.append(stripped)

        if ";" in stripped:
            statement = normalize_ws(" ".join(pending))
            pending = []
            if re.search(r"\bModu_[A-Za-z0-9_]+\s*\(", statement):
                sig = statement.rstrip(";")
                doc = extract_inline_doc(lines, pending_start)
                items.append(ApiItem(group, sig, doc))

    return items


def parse_cs_class_members(lines: List[str], class_decl_pattern: str, default_group: str) -> List[ApiItem]:
    class_re = re.compile(class_decl_pattern)
    start = None
    for i, line in enumerate(lines):
        if class_re.search(line):
            start = i
            break
    if start is None:
        raise RuntimeError(f"Unable to find class pattern: {class_decl_pattern}")

    items: List[ApiItem] = []
    depth = lines[start].count("{") - lines[start].count("}")
    pending: List[str] = []
    pending_start = -1

    for idx in range(start + 1, len(lines)):
        line = lines[idx]
        stripped = line.strip()
        current_depth = depth
        if current_depth <= 0:
            break

        if current_depth == 1:
            if not pending:
                if stripped.startswith("public "):
                    pending = [stripped]
                    pending_start = idx
                    if "{" in stripped or stripped.endswith(";"):
                        statement = normalize_ws(" ".join(pending))
                        pending = []
                        doc = extract_inline_doc(lines, pending_start)
                        statement = statement.split("{", 1)[0].strip()
                        if statement.endswith(";"):
                            signature = statement.rstrip(";")
                        else:
                            signature = f"{statement} {{ ... }}"
                        items.append(ApiItem(default_group, signature, doc))
                depth += line.count("{") - line.count("}")
                continue
            else:
                pending.append(stripped)
                if "{" in stripped or stripped.endswith(";"):
                    statement = normalize_ws(" ".join(pending))
                    pending = []
                    doc = extract_inline_doc(lines, pending_start)
                    statement = statement.split("{", 1)[0].strip()
                    if statement.endswith(";"):
                        signature = statement.rstrip(";")
                    else:
                        signature = f"{statement} {{ ... }}"
                    items.append(ApiItem(default_group, signature, doc))

        depth += line.count("{") - line.count("}")

    return items


def parse_cs_attributes(lines: List[str]) -> List[ApiItem]:
    items: List[ApiItem] = []
    for idx, line in enumerate(lines):
        stripped = line.strip()
        m = re.match(r"public\s+sealed\s+class\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*Attribute", stripped)
        if not m:
            continue
        name = m.group(1)
        doc = extract_inline_doc(lines, idx)
        items.append(ApiItem("C# Attributes", f"class {name} : Attribute", doc))
    return items


def build_generated_reference(root: Path) -> str:
    cpp_header = root / "src" / "ScriptRuntime.h"
    c_header = root / "include" / "ScriptRuntimeCAPI.h"
    cs_file = root / "Scripts" / "Managed" / "ModuCPP.cs"

    cpp_fields, cpp_methods = parse_script_context(cpp_header)
    c_api = parse_c_api(c_header)
    cs_lines = cs_file.read_text(encoding="utf-8").splitlines()
    cs_context = parse_cs_class_members(
        cs_lines, r"public\s+readonly\s+unsafe\s+struct\s+Context\s*\{", "C# Context Members"
    )
    cs_imgui = parse_cs_class_members(
        cs_lines, r"public\s+static\s+unsafe\s+class\s+ImGui\s*\{", "C# ImGui Members"
    )
    cs_inspector = parse_cs_class_members(
        cs_lines, r"public\s+static\s+class\s+Inspector\s*\{", "C# Inspector Members"
    )
    cs_attributes = parse_cs_attributes(cs_lines)

    out: List[str] = []
    out.append(
        "_Generated from source. Add notes with `///` or `// @doc:` above declarations. "
        "Supported tags: `@summary`, `@usage`, `@howto`, `@param`, `@returns`, `@note`, `@example`/`@endexample`._"
    )
    out.append("")
    out.append("### C++ `ScriptContext` Fields")
    out.extend(render_grouped_items(cpp_fields))
    out.append("### C++ `ScriptContext` Methods")
    out.extend(render_grouped_items(cpp_methods))
    out.append("### C `Modu_*` API")
    out.extend(render_grouped_items(c_api))
    out.append("### C# `ModuCPP` API")
    out.extend(render_grouped_items(cs_context))
    out.extend(render_grouped_items(cs_imgui))
    out.extend(render_grouped_items(cs_inspector))
    out.extend(render_grouped_items(cs_attributes))

    return "\n".join(out).rstrip() + "\n"


def replace_generated_block(doc_text: str, generated_block: str) -> str:
    start_idx = doc_text.find(MARKER_START)
    end_idx = doc_text.find(MARKER_END)
    if start_idx < 0 or end_idx < 0 or end_idx < start_idx:
        raise RuntimeError(
            "Missing auto-gen markers in docs/Scripting.md. "
            f"Expected '{MARKER_START}' and '{MARKER_END}'."
        )
    prefix = doc_text[: start_idx + len(MARKER_START)]
    suffix = doc_text[end_idx:]
    return f"{prefix}\n\n{generated_block}\n{suffix}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate scripting API reference in docs/Scripting.md")
    parser.add_argument("--check", action="store_true", help="Do not write; fail if docs are out of date")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    docs_path = root / "docs" / "Scripting.md"
    current = docs_path.read_text(encoding="utf-8")
    generated = build_generated_reference(root)
    updated = replace_generated_block(current, generated)

    if args.check:
        if updated != current:
            print("docs/Scripting.md is out of date. Run: python3 tools/generate_scripting_docs.py", file=sys.stderr)
            return 1
        print("docs/Scripting.md is up to date.")
        return 0

    if updated != current:
        docs_path.write_text(updated, encoding="utf-8")
        print("Updated docs/Scripting.md")
    else:
        print("No changes (docs already up to date)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
