#!/usr/bin/env python3
"""Visualize glyph bitmaps from generated MorseMatrix font C++ files."""

from __future__ import annotations

import argparse
import enum
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional


class Orientation(enum.Enum):
    """Indicates how bits are stored inside each glyph."""

    ROW_MAJOR_MSB_FIRST = "row-major-msb"
    ROW_MAJOR_LSB_FIRST = "row-major-lsb"
    COL_MAJOR_TOP_LSB = "col-major-top-lsb"
    COL_MAJOR_TOP_MSB = "col-major-top-msb"


@dataclass
class Glyph:
    codepoint: int
    width: int
    height: int
    stride: int
    bitmap_rows: List[int]

    def render(self, orientation: Orientation) -> List[str]:
        rows = ["." * self.width for _ in range(self.height)]

        # Convert strings to mutable lists for efficient updates.
        rows_mut = [list(row) for row in rows]

        if orientation == Orientation.ROW_MAJOR_MSB_FIRST:
            for row in range(self.height):
                row_bytes = self.bitmap_rows[row * self.stride : (row + 1) * self.stride]
                for col in range(self.width):
                    byte_idx = col // 8
                    bit_idx = 7 - (col % 8)
                    if row_bytes[byte_idx] & (1 << bit_idx):
                        rows_mut[row][col] = "X"
        elif orientation == Orientation.ROW_MAJOR_LSB_FIRST:
            for row in range(self.height):
                row_bytes = self.bitmap_rows[row * self.stride : (row + 1) * self.stride]
                for col in range(self.width):
                    byte_idx = col // 8
                    bit_idx = col % 8
                    if row_bytes[byte_idx] & (1 << bit_idx):
                        rows_mut[row][col] = "X"
        elif orientation in (Orientation.COL_MAJOR_TOP_LSB, Orientation.COL_MAJOR_TOP_MSB):
            for col in range(self.width):
                col_bytes = self.bitmap_rows[col * self.stride : (col + 1) * self.stride]
                for row in range(self.height):
                    bit_idx = row % 8
                    byte_idx = row // 8
                    if byte_idx >= len(col_bytes):
                        break
                    if orientation == Orientation.COL_MAJOR_TOP_LSB:
                        bit_mask = 1 << bit_idx
                    else:
                        bit_mask = 1 << (7 - bit_idx)
                    if col_bytes[byte_idx] & bit_mask:
                        rows_mut[row][col] = "X"
        else:
            raise ValueError(f"Unsupported orientation: {orientation}")

        return ["".join(row) for row in rows_mut]


@dataclass
class FontDefinition:
    name: str
    default_width: int
    default_height: int
    default_stride: int
    glyphs: Dict[int, Glyph]


FONT_STRUCT_PATTERN = re.compile(
    r"const\s+m5_morsematrix_font_t\s+(?P<sym>g_[^\s=]+)\s*=\s*\{"
    r"\s*\"(?P<name>[^\"]+)\"\s*,\s*(?P<width>\d+)\s*,\s*(?P<height>\d+)\s*,\s*(?P<stride>\d+)",
    re.MULTILINE,
)

GLYPH_ARRAY_PATTERN = re.compile(
    r"const\s+m5_morsematrix_glyph_t\s+(?P<sym>g_[^\s=]+)_glyphs\[\]\s*=\s*\{",
    re.MULTILINE,
)

GLYPH_ENTRY_PATTERN = re.compile(
    r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*&([^\s\}]+)\s*\}",
)

BITMAP_PATTERN = re.compile(
    r"static\s+const\s+uint8_t\s+(?P<name>glyph_[^\s]+)_bitmap\s*\[\s*(?P<outer>\d+)\s*\]\s*\[\s*(?P<inner>\d+)\s*\]\s*=\s*\{(?P<body>.*?)\};",
    re.DOTALL,
)


def parse_bitmap_arrays(source: str) -> Dict[str, List[int]]:
    bitmaps: Dict[str, List[int]] = {}
    for match in BITMAP_PATTERN.finditer(source):
        name = match.group("name") + "_bitmap"
        outer = int(match.group("outer"))
        inner = int(match.group("inner"))
        body = match.group("body")

        values: List[int] = []
        for raw_line in body.splitlines():
            raw_line = raw_line.strip()
            if not raw_line:
                continue
            raw_line = raw_line.strip("{},")
            if not raw_line:
                continue
            try:
                values.append(int(raw_line, 16))
            except ValueError:
                print(f"Warning: failed to parse bitmap value '{raw_line}' in {name}", file=sys.stderr)
                continue

        expected = outer * inner
        if len(values) != expected:
            print(
                f"Warning: bitmap {name} expected {expected} values but found {len(values)}",
                file=sys.stderr,
            )

        bitmaps[name] = values
    return bitmaps


def parse_font_definition(source: str) -> Optional[FontDefinition]:
    font_match = FONT_STRUCT_PATTERN.search(source)
    glyph_array_match = GLYPH_ARRAY_PATTERN.search(source)
    if not font_match or not glyph_array_match:
        return None

    font_name = font_match.group("name")
    default_width = int(font_match.group("width"))
    default_height = int(font_match.group("height"))
    default_stride = int(font_match.group("stride"))

    glyphs: Dict[int, Glyph] = {}
    glyph_table_section = source[glyph_array_match.end() :]
    bitmap_values = parse_bitmap_arrays(source)

    for entry_match in GLYPH_ENTRY_PATTERN.finditer(glyph_table_section):
        codepoint = int(entry_match.group(1), 16)
        width = int(entry_match.group(2))
        height = int(entry_match.group(3))
        stride = int(entry_match.group(4))
        bitmap_ref = entry_match.group(5)

        bitmap_name = bitmap_ref.lstrip("&")
        if bitmap_name.endswith("[0][0]"):
            bitmap_name = bitmap_name[:-6]
        bitmap_name = bitmap_name.strip()
        bitmap_data = bitmap_values.get(bitmap_name)
        if not bitmap_data:
            print(f"Warning: bitmap data for {bitmap_name} not found", file=sys.stderr)
            continue

        glyphs[codepoint] = Glyph(
            codepoint=codepoint,
            width=width,
            height=height,
            stride=stride,
            bitmap_rows=bitmap_data,
        )

    return FontDefinition(
        name=font_name,
        default_width=default_width,
        default_height=default_height,
        default_stride=default_stride,
        glyphs=glyphs,
    )


def readable_codepoint(codepoint: int) -> str:
    try:
        char = chr(codepoint)
        if char.isprintable():
            return f"'{char}' (U+{codepoint:04X})"
    except ValueError:
        pass
    return f"U+{codepoint:04X}"


def visualize_glyph(glyph: Glyph, orientation: Orientation) -> None:
    lines = glyph.render(orientation)
    print(f"\n{readable_codepoint(glyph.codepoint)}:")
    for idx, row_bits in enumerate(lines):
        raw_value = glyph.bitmap_rows[idx] if idx < len(glyph.bitmap_rows) else 0
        print(f"Row{idx}: {row_bits}  (0x{raw_value:02X})")


def parse_orientation(value: str) -> Orientation:
    try:
        return Orientation(value)
    except ValueError as exc:
        valid = ", ".join(o.value for o in Orientation)
        raise argparse.ArgumentTypeError(f"Invalid orientation '{value}'. Valid options: {valid}") from exc


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font_cpp", type=Path, help="Path to generated font .cpp file")
    parser.add_argument(
        "-g",
        "--glyph",
        dest="glyphs",
        action="append",
        help="Glyph(s) to render. Accepts character literals or hex codepoints (e.g. 'A' or 0x41). Can be repeated.",
    )
    parser.add_argument(
        "-o",
        "--orientation",
        type=parse_orientation,
        default=Orientation.ROW_MAJOR_MSB_FIRST,
        help="Bitmap orientation; defaults to row-major-msb (ESP32 glyphs).",
    )
    parser.add_argument(
        "-l",
        "--list",
        action="store_true",
        help="List available glyphs instead of rendering.",
    )
    return parser


def resolve_requested_glyphs(font: FontDefinition, requests: Optional[Iterable[str]]) -> List[Glyph]:
    if not requests:
        return sorted(font.glyphs.values(), key=lambda g: g.codepoint)

    resolved: List[Glyph] = []
    for item in requests:
        item = item.strip()
        codepoint: Optional[int] = None

        if len(item) == 1:
            codepoint = ord(item)
        else:
            if item.lower().startswith("0x"):
                try:
                    codepoint = int(item, 16)
                except ValueError:
                    pass
            if codepoint is None:
                print(f"Warning: cannot interpret glyph identifier '{item}'", file=sys.stderr)
                continue

        glyph = font.glyphs.get(codepoint)
        if not glyph:
            print(f"Warning: glyph {readable_codepoint(codepoint)} not found in font", file=sys.stderr)
            continue
        resolved.append(glyph)

    return resolved


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if not args.font_cpp.exists():
        parser.error(f"Font file not found: {args.font_cpp}")

    source = args.font_cpp.read_text(encoding="utf-8")
    font = parse_font_definition(source)
    if not font:
        parser.error("Could not parse font from the specified file")

    if args.list:
        print(f"Font '{font.name}' (default {font.default_width}x{font.default_height}, stride={font.default_stride})")
        for glyph in sorted(font.glyphs.values(), key=lambda g: g.codepoint):
            print(f"  {readable_codepoint(glyph.codepoint)} -> {glyph.width}x{glyph.height}, stride={glyph.stride}")
        return 0

    glyphs = resolve_requested_glyphs(font, args.glyphs)
    if not glyphs:
        parser.error("No glyphs resolved; use --list to inspect available glyphs or specify valid identifiers")

    for glyph in glyphs:
        visualize_glyph(glyph, args.orientation)

    return 0


if __name__ == "__main__":
    sys.exit(main())
