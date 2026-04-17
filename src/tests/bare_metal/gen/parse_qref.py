#!/usr/bin/env python3
"""Parse s1c33000_quick_reference.md instruction tables into structured entries.

Extracts per-row (mnemonic, form, encoding, semantics, cycles, flag_effects,
ext_max, delayable, section).  The input markdown is not perfectly regular, so
this parser accepts best-effort and reports unparsed rows so the user can see
where hand-correction is required.

Run:  parse_qref.py [path_to_quick_reference.md]  → dumps JSON on stdout
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

DEFAULT_PATH = Path("/home/autch/src/llvm-c33/docs/s1c33000_quick_reference.md")

# Section headings whose tables describe actual instructions
INSTR_SECTIONS = {
    "データ転送",
    "論理演算",
    "算術演算",
    "シフト&ローテート",
    "ビット操作",
    "即値拡張",
    "プッシュ&ポップ",
    "分岐",
    "積和演算",
    "システム制御",
    "その他",
}


@dataclass
class Entry:
    section: str
    mnemonic: str
    operand: str
    encoding: str
    semantics: str
    cycles: str
    flags: str          # 6-char string like "----↔↔" (MO,DS,C,V,Z,N)
    ext_max: int        # 0 if no ext support
    delayable: bool
    row_no: int         # line number in source file
    warnings: list[str] = field(default_factory=list)


def normalise_cell(cell: str) -> str:
    return cell.replace("\x00", "|").strip()


_PIPE_PLACEHOLDER = "\x00"


def split_md_row(line: str) -> list[str]:
    # Replace escaped pipe \| with NUL placeholder before splitting,
    # then restore inside normalise_cell.
    masked = line.replace(r"\|", _PIPE_PLACEHOLDER)
    return [normalise_cell(p) for p in masked.split("|")[1:-1]]


def parse_flags(raw: str) -> str:
    # Accept "------", "--↔↔↔↔", "↔↔↔↔↔↔", "-↔---↔", etc.
    # Width must be 6; pad if shorter.
    s = raw.strip()
    if len(s) == 6:
        return s
    # Some rows have "----↔↔" with stray spaces.
    compact = re.sub(r"\s+", "", s)
    if len(compact) == 6:
        return compact
    return s  # return as-is; caller will warn


def parse_ext(raw: str) -> int:
    s = raw.strip()
    if s in {"-", ""}:
        return 0
    try:
        return int(s)
    except ValueError:
        return -1  # unknown


def parse_delayable(raw: str) -> Optional[bool]:
    s = raw.strip()
    if s in {"○", "o", "O"}:
        return True
    if s in {"-", ""}:
        return False
    return None


def parse_md(path: Path) -> list[Entry]:
    lines = path.read_text(encoding="utf-8").splitlines()

    entries: list[Entry] = []
    section: Optional[str] = None
    in_table = False
    # When a data row has an empty first cell, mnemonic carries over
    last_mnemonic: Optional[str] = None
    header_cols: list[str] = []

    for i, line in enumerate(lines, start=1):
        # Section heading
        m = re.match(r"^###\s+(.+?)\s*$", line)
        if m:
            section = m.group(1).strip()
            in_table = False
            last_mnemonic = None
            continue

        # Leave table context on blank or non-pipe line
        if in_table and (not line.strip() or not line.lstrip().startswith("|")):
            in_table = False
            last_mnemonic = None

        if section not in INSTR_SECTIONS:
            continue

        if not line.lstrip().startswith("|"):
            continue

        # Parse a markdown table row
        parts = split_md_row(line)
        if not parts:
            continue

        # Header row detection
        if not in_table:
            if "オペコード" in parts[0]:
                header_cols = parts
                in_table = True
                continue
            # else: not a table we care about
            continue

        # Separator row like |---|---|
        if all(re.match(r"^:?-+:?$", p) for p in parts):
            continue

        # Data row
        if len(parts) < 8:
            # Some rows (delayed-suffix placeholders like jrgt.d) have all empty cells
            # Skip silently; they're documentation artefacts
            continue

        mnemonic = parts[0]
        operand = parts[1]
        encoding = parts[2]
        semantics = parts[3]
        cycles = parts[4]
        flags_raw = parts[5]
        ext_raw = parts[6]
        delay_raw = parts[7]

        if not mnemonic:
            if last_mnemonic is None:
                continue
            mnemonic = last_mnemonic
        else:
            last_mnemonic = mnemonic

        # Skip placeholder rows where the encoding is empty
        if not encoding:
            continue

        warnings: list[str] = []
        flags = parse_flags(flags_raw)
        if len(re.sub(r"\s", "", flags)) != 6:
            warnings.append(f"flags column unrecognised: {flags_raw!r}")

        ext_max = parse_ext(ext_raw)
        if ext_max < 0:
            warnings.append(f"ext column unrecognised: {ext_raw!r}")
            ext_max = 0

        delayable = parse_delayable(delay_raw)
        if delayable is None:
            warnings.append(f"delay column unrecognised: {delay_raw!r}")
            delayable = False

        entries.append(Entry(
            section=section,
            mnemonic=mnemonic,
            operand=operand,
            encoding=encoding,
            semantics=semantics,
            cycles=cycles,
            flags=flags,
            ext_max=ext_max,
            delayable=delayable,
            row_no=i,
            warnings=warnings,
        ))

    return entries


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PATH
    entries = parse_md(path)

    # Report
    n_total = len(entries)
    n_warn = sum(1 for e in entries if e.warnings)
    print(f"# parsed {n_total} entries, {n_warn} with warnings", file=sys.stderr)

    # Group summary
    by_section: dict[str, int] = {}
    for e in entries:
        by_section[e.section] = by_section.get(e.section, 0) + 1
    for sec, n in by_section.items():
        print(f"#   {sec}: {n}", file=sys.stderr)

    json.dump([asdict(e) for e in entries], sys.stdout, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
