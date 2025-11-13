"""Font matrix generator for M5 Morse Matrix LED board.

This script reads character lists for ASCII, Chinese, and Japanese glyph
collections, renders them using Windows-installed fonts (via Pillow), and
emits C++ source/header files that contain bitmap data in a charlieplex-friendly
format. The generator supports arbitrary matrix dimensions, such as 8x8, 6x8,
or 4x6, by scanning the output bitmap row-by-row.

Usage example (PowerShell):

	python m5_morsematrix_fm_toolbox.py \
		--width 8 --height 8 \
		--ascii-font Consolas \
		--cn-font "Microsoft YaHei" \
		--jp-font "Yu Gothic" \
		--ascii-file m5_morsematrix_fm_ascii_text.txt \
		--cn-file m5_morsematrix_fm_cn_text.txt \
		--jp-file m5_morsematrix_fm_jp_text.txt \
		--output-cpp m5_morsematrix_fm_normal_8x8.cpp \
		--output-h m5_morsematrix_fm_normal_8x8.h

Requirements:
	pip install Pillow

The script attempts to locate fonts by either absolute path or by family/file
name inside the Windows fonts directory. If specific CN/JP fonts are not
provided, they default to the ASCII font selection.
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence

try:
	from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:  # pragma: no cover - user environment issue
	raise SystemExit(
		"Pillow is required. Install it with 'pip install Pillow'"
	) from exc


# ---------------------------------------------------------------------------
# Data structures

@dataclass
class Glyph:
	"""Rendered glyph information."""

	char: str
	codepoint: int
	width: int
	height: int
	stride: int
	bitmap: List[int]


# ---------------------------------------------------------------------------
# Character list helpers

def read_character_file(path: Path) -> List[str]:
	"""Parse a character list file into individual glyphs.

	The function keeps the original order and allows duplicate characters. Lines
	starting with '#' are treated as comments. Each non-comment line contributes
	all of its characters (including spaces).
	"""

	if not path.exists():
		return []

	chars: List[str] = []
	with path.open("r", encoding="utf-8") as handle:
		for line in handle:
			if line.startswith("#"):
				continue
			line = line.rstrip("\n")
			if not line:
				continue
			chars.extend(list(line))
	unique = OrderedDict((ch, None) for ch in chars)
	return list(unique.keys())


def merge_char_sequences(sequences: Sequence[Sequence[str]]) -> List[str]:
	"""Merge character sequences while preserving order and removing duplicates."""

	ordered: "OrderedDict[str, None]" = OrderedDict()
	for seq in sequences:
		for ch in seq:
			if ch not in ordered:
				ordered[ch] = None
	return list(ordered.keys())


# ---------------------------------------------------------------------------
# Font discovery and rendering helpers

def resolve_font_path(font_spec: str | None) -> str:
	"""Resolve a font specification to an absolute path.

	The function accepts:
	  * Absolute or relative file paths.
	  * File names located inside the Windows Fonts directory.
	  * Font family names (best effort search by prefix, case-insensitive).
	"""

	if not font_spec:
		raise ValueError("Font specification must not be empty")

	candidate = Path(font_spec)
	if candidate.exists():
		return str(candidate.resolve())

	fonts_dir = Path(Path.home().drive or "C:") / "Windows" / "Fonts"
	if not fonts_dir.exists():
		raise FileNotFoundError(f"Windows fonts directory not found: {fonts_dir}")

	# If user passed file name with extension, try direct match first.
	if candidate.suffix:
		matches = list(fonts_dir.glob(font_spec))
		if matches:
			return str(matches[0].resolve())

	# Attempt case-insensitive prefix match for family names.
	def normalize(value: str) -> str:
		return value.lower().replace(" ", "").replace("-", "")

	lowered = font_spec.lower()
	normalized_spec = normalize(font_spec)
	for extension in ("*.ttf", "*.ttc", "*.otf"):  # common font containers
		for item in fonts_dir.glob(extension):
			stem_lower = item.stem.lower()
			if stem_lower.startswith(lowered) or lowered.startswith(stem_lower):
				return str(item.resolve())
			if normalize(item.stem) == normalized_spec:
				return str(item.resolve())

	raise FileNotFoundError(f"Unable to locate font: {font_spec}")


def load_font(font_path: str, size: int) -> ImageFont.FreeTypeFont:
	"""Load a TrueType/OpenType font."""

	try:
		return ImageFont.truetype(font_path, size=size)
	except OSError as exc:  # pragma: no cover - user environment issue
		raise SystemExit(f"Failed to load font '{font_path}': {exc}") from exc


def render_glyph(
	font: ImageFont.FreeTypeFont,
	ch: str,
	width: int,
	height: int,
	threshold: int = 128,
	scale_factor: int = 16,
	padding: int = 1,
	pixel_threshold: int = 20,
	adaptive_percentile: int = 33,
	min_adaptive_threshold: int = 80,
) -> Glyph:
	"""Render a single glyph into a bitmap.

	The bitmap is encoded row-by-row, MSB-first, with stride bytes per scanline.
	
	Strategy:
	1. Render character at very high resolution to get accurate shape
	2. Find tight bounding box of actual rendered pixels
	3. Scale down to target size, filling it completely
	4. Apply threshold to convert to 1-bit bitmap
	
	Args:
		font: FreeType font object
		ch: Character to render
		width: Target width in pixels
		height: Target height in pixels
		threshold: Fixed threshold (fallback, not used in adaptive mode)
		scale_factor: High-resolution rendering scale (default 16)
		padding: Padding around glyph bounding box (default 1)
		pixel_threshold: Threshold for detecting glyph bounds (default 20)
		adaptive_percentile: Percentile for adaptive thresholding (default 33)
		min_adaptive_threshold: Minimum threshold value (default 80)
	"""

	# 1. 使用非常高的分辨率渲染以获得最佳质量
	# 对于8x8目标，使用128x128高分辨率画布（16倍）
	hires_size = max(width, height) * scale_factor
	
	# 创建高分辨率画布
	hires_canvas = Image.new("L", (hires_size, hires_size), color=0)
	hires_draw = ImageDraw.Draw(hires_canvas)
	
	# 获取文字边界框（用于定位）
	bbox = hires_draw.textbbox((hires_size // 4, hires_size // 4), ch, font=font)
	
	# 如果边界框无效，返回空字形
	if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
		canvas = Image.new("L", (width, height), color=0)
		stride = math.ceil(width / 8)
		bitmap: List[int] = [0] * (height * stride)
		return Glyph(
			char=ch,
			codepoint=ord(ch),
			width=width,
			height=height,
			stride=stride,
			bitmap=bitmap,
		)
	
	# 在画布中心渲染字符
	center_x = hires_size // 2
	center_y = hires_size // 2
	char_width = bbox[2] - bbox[0]
	char_height = bbox[3] - bbox[1]
	
	# 居中绘制
	draw_x = center_x - char_width // 2 - bbox[0]
	draw_y = center_y - char_height // 2 - bbox[1]
	hires_draw.text((draw_x, draw_y), ch, fill=255, font=font)
	
	# 2. 扫描画布，找出实际有像素的紧密边界
	pixels = hires_canvas.load()
	min_x, min_y = hires_size, hires_size
	max_x, max_y = -1, -1
	
	# Use pixel_threshold parameter
	
	for y in range(hires_size):
		for x in range(hires_size):
			if pixels[x, y] > pixel_threshold:
				min_x = min(min_x, x)
				min_y = min(min_y, y)
				max_x = max(max_x, x)
				max_y = max(max_y, y)
	
	# 如果没找到像素，返回空字形
	if max_x < 0:
		canvas = Image.new("L", (width, height), color=0)
		stride = math.ceil(width / 8)
		bitmap: List[int] = [0] * (height * stride)
		return Glyph(
			char=ch,
			codepoint=ord(ch),
			width=width,
			height=height,
			stride=stride,
			bitmap=bitmap,
		)
	
	# 3. 裁剪紧密边界，添加padding像素防止边缘被截断
	# Use padding parameter
	crop_x1 = max(0, min_x - padding)
	crop_y1 = max(0, min_y - padding)
	crop_x2 = min(hires_size, max_x + 1 + padding)
	crop_y2 = min(hires_size, max_y + 1 + padding)
	
	char_image = hires_canvas.crop((crop_x1, crop_y1, crop_x2, crop_y2))
	
	# 4. 直接缩放到目标尺寸（填满整个空间）
	# 使用LANCZOS获得最佳的下采样质量
	char_image = char_image.resize((width, height), Image.Resampling.LANCZOS)
	
	# 5. 转换为1位位图
	stride = math.ceil(width / 8)
	bitmap = []
	final_pixels = char_image.load()
	
	# 使用自适应阈值：使用指定的百分位数
	pixel_values = [final_pixels[x, y] for y in range(height) for x in range(width) if final_pixels[x, y] > 0]
	if pixel_values:
		pixel_values.sort()
		# Fix: Correctly calculate percentile index
		if adaptive_percentile >= 100:
			percentile_idx = len(pixel_values) - 1
		else:
			percentile_idx = min(int(len(pixel_values) * adaptive_percentile / 100.0), len(pixel_values) - 1)
		adaptive_threshold = pixel_values[percentile_idx]
		adaptive_threshold = max(adaptive_threshold, min_adaptive_threshold)
	else:
		adaptive_threshold = min_adaptive_threshold

	for y in range(height):
		for byte_index in range(stride):
			value = 0
			for bit in range(8):
				x = byte_index * 8 + bit
				if x >= width:
					continue
				if final_pixels[x, y] >= adaptive_threshold:
					value |= 1 << (7 - bit)
			bitmap.append(value)

	return Glyph(
		char=ch,
		codepoint=ord(ch),
		width=width,
		height=height,
		stride=stride,
		bitmap=bitmap,
	)


def build_glyphs(
	chars: Iterable[str],
	group_fonts: Dict[str, ImageFont.FreeTypeFont],
	char_groups: Dict[str, str],
	width: int,
	height: int,
	scale_factor: int = 16,
	padding: int = 1,
	pixel_threshold: int = 20,
	adaptive_percentile: int = 33,
	min_adaptive_threshold: int = 80,
	threshold: int = 128,
) -> List[Glyph]:
	"""Render all requested glyphs with group-specific fonts."""

	glyphs: List[Glyph] = []
	for ch in chars:
		group = char_groups.get(ch, "ascii")
		font = group_fonts[group]
		glyphs.append(render_glyph(
			font, ch, width, height,
			threshold=threshold,
			scale_factor=scale_factor,
			padding=padding,
			pixel_threshold=pixel_threshold,
			adaptive_percentile=adaptive_percentile,
			min_adaptive_threshold=min_adaptive_threshold,
		))
	return glyphs


# ---------------------------------------------------------------------------
# Code generation helpers

CPP_HEADER = """// Auto-generated by m5_morsematrix_fm_toolbox.py. DO NOT EDIT.
#include <cstddef>
#include "{header_name}"

namespace {{
{glyph_bitmaps}
}} // namespace

const m5_morsematrix_glyph_t g_m5_morsematrix_font_{font_symbol}_glyphs[] = {{
{glyph_table}
}};

const size_t g_m5_morsematrix_font_{font_symbol}_count =
	sizeof(g_m5_morsematrix_font_{font_symbol}_glyphs) / sizeof(g_m5_morsematrix_font_{font_symbol}_glyphs[0]);

const m5_morsematrix_font_t g_m5_morsematrix_font_{font_symbol} = {{
	"{font_name}",
	{default_width},
	{default_height},
	{default_stride},
	g_m5_morsematrix_font_{font_symbol}_glyphs,
	g_m5_morsematrix_font_{font_symbol}_count
}};
"""


H_HEADER = """// Auto-generated by m5_morsematrix_fm_toolbox.py. DO NOT EDIT.
#pragma once

#include <cstddef>
#include <cstdint>

typedef struct {{
	uint32_t codepoint;  // Unicode code point
	uint8_t width;       // Glyph pixel width
	uint8_t height;      // Glyph pixel height
	uint8_t stride;      // Bytes per row (ceil(width/8))
	const uint8_t* bitmap;  // Pointer to row-major bitmap data (2D array flattened)
}} m5_morsematrix_glyph_t;

typedef struct {{
	const char* name;           // Font name (e.g., "normal_8x8")
	uint8_t default_width;      // Default glyph width
	uint8_t default_height;     // Default glyph height
	uint8_t default_stride;     // Default stride
	const m5_morsematrix_glyph_t* glyphs;
	size_t glyph_count;
}} m5_morsematrix_font_t;

extern const m5_morsematrix_glyph_t g_m5_morsematrix_font_{font_symbol}_glyphs[];
extern const size_t g_m5_morsematrix_font_{font_symbol}_count;
extern const m5_morsematrix_font_t g_m5_morsematrix_font_{font_symbol};
"""


def format_bitmap(glyph: Glyph, font_symbol: str) -> str:
	"""Format bitmap bytes into a 2D C++ array literal."""

	bytes_per_row = glyph.stride
	rows: List[str] = []
	for row in range(glyph.height):
		start = row * bytes_per_row
		row_bytes = glyph.bitmap[start : start + bytes_per_row]
		row_hex = ", ".join(f"0x{value:02X}" for value in row_bytes)
		rows.append(f"    {{ {row_hex} }}")
	joined = ",\n".join(rows)
	return (
		f"static const uint8_t glyph_{font_symbol}_{glyph.codepoint:04X}_bitmap[{glyph.height}][{glyph.stride}] = {{\n{joined}\n}};"
	)


def emit_cpp(header_name: str, glyphs: Sequence[Glyph], font_name: str, font_symbol: str, default_width: int, default_height: int, default_stride: int) -> str:
	glyph_bitmaps = "\n\n".join(format_bitmap(g, font_symbol) for g in glyphs)
	table_entries = []
	for glyph in glyphs:
		table_entries.append(
			"    { 0x%04X, %d, %d, %d, &glyph_%s_%04X_bitmap[0][0] }" % (
				glyph.codepoint,
				glyph.width,
				glyph.height,
				glyph.stride,
				font_symbol,
				glyph.codepoint,
			)
		)
	glyph_table = ",\n".join(table_entries)
	return CPP_HEADER.format(
		header_name=header_name,
		glyph_bitmaps=glyph_bitmaps,
		glyph_table=glyph_table,
		font_name=font_name,
		font_symbol=font_symbol,
		default_width=default_width,
		default_height=default_height,
		default_stride=default_stride
	)


def emit_header(font_symbol: str) -> str:
	return H_HEADER.format(font_symbol=font_symbol)


# ---------------------------------------------------------------------------
# CLI

def parse_args(argv: Sequence[str]) -> argparse.Namespace:
	parser = argparse.ArgumentParser(description="Generate Morse Matrix font bitmaps")
	parser.add_argument("--width", type=int, default=8, help="Glyph width in pixels")
	parser.add_argument("--height", type=int, default=8, help="Glyph height in pixels")
	parser.add_argument(
		"--font-size",
		type=int,
		default=None,
		help="Font size passed to FreeType (defaults to height)",
	)
	# Rendering parameters
	parser.add_argument(
		"--scale-factor",
		type=int,
		default=16,
		help="High-resolution rendering scale factor (default: 16)",
	)
	parser.add_argument(
		"--padding",
		type=int,
		default=1,
		help="Padding around glyph bounding box (default: 1)",
	)
	parser.add_argument(
		"--pixel-threshold",
		type=int,
		default=20,
		help="Threshold for detecting glyph bounds (default: 20)",
	)
	parser.add_argument(
		"--adaptive-percentile",
		type=int,
		default=33,
		help="Percentile for adaptive thresholding (default: 33)",
	)
	parser.add_argument(
		"--min-adaptive-threshold",
		type=int,
		default=80,
		help="Minimum adaptive threshold value (default: 80)",
	)
	parser.add_argument(
		"--threshold",
		type=int,
		default=128,
		help="Fixed threshold (unused in adaptive mode, default: 128)",
	)
	# Font parameters
	parser.add_argument(
		"--ascii-font",
		type=str,
		default="Consolas",
		help="Font for ASCII characters (family name or path)",
	)
	parser.add_argument(
		"--cn-font",
		type=str,
		default=None,
		help="Font for Chinese characters (defaults to ASCII font)",
	)
	parser.add_argument(
		"--jp-font",
		type=str,
		default=None,
		help="Font for Japanese characters (defaults to ASCII font)",
	)
	parser.add_argument(
		"--ascii-file",
		type=Path,
		default=Path("m5_morsematrix_fm_ascii_text.txt"),
		help="Path to ASCII character list",
	)
	parser.add_argument(
		"--cn-file",
		type=Path,
		default=Path("m5_morsematrix_fm_cn_text.txt"),
		help="Path to Chinese character list",
	)
	parser.add_argument(
		"--jp-file",
		type=Path,
		default=Path("m5_morsematrix_fm_jp_text.txt"),
		help="Path to Japanese character list",
	)
	parser.add_argument(
		"--output-cpp",
		type=Path,
		default=Path("m5_morsematrix_fm_normal_8x8.cpp"),
		help="Output C++ source file",
	)
	parser.add_argument(
		"--output-h",
		type=Path,
		default=Path("m5_morsematrix_fm_normal_8x8.h"),
		help="Output C++ header file",
	)
	parser.add_argument(
		"--font-name",
		type=str,
		default=None,
		help="Font name (defaults to output filename without prefix)",
	)
	return parser.parse_args(argv)


def derive_font_info(output_cpp: Path, font_name_override: str | None) -> tuple[str, str]:
	"""Derive font name and symbol from output filename."""
	if font_name_override:
		font_name = font_name_override
	else:
		stem = output_cpp.stem
		# Remove "m5_morsematrix_fm_" prefix if present
		if stem.startswith("m5_morsematrix_fm_"):
			font_name = stem[18:]  # len("m5_morsematrix_fm_") = 18
		else:
			font_name = stem
	
	# Create C-safe symbol name
	import re
	font_symbol = re.sub(r'[^a-zA-Z0-9_]', '_', font_name)
	
	return font_name, font_symbol


def main(argv: Sequence[str] | None = None) -> int:
	args = parse_args(argv or sys.argv[1:])

	font_size = args.font_size or args.height

	ascii_chars = read_character_file(args.ascii_file)
	cn_chars = read_character_file(args.cn_file)
	jp_chars = read_character_file(args.jp_file)

	all_chars = merge_char_sequences([ascii_chars, cn_chars, jp_chars])
	if not all_chars:
		print("No characters found in input files.", file=sys.stderr)
		return 1

	ascii_font_path = resolve_font_path(args.ascii_font)
	cn_font_path = resolve_font_path(args.cn_font) if args.cn_font else ascii_font_path
	jp_font_path = resolve_font_path(args.jp_font) if args.jp_font else ascii_font_path

	fonts = {
		"ascii": load_font(ascii_font_path, font_size),
		"cn": load_font(cn_font_path, font_size),
		"jp": load_font(jp_font_path, font_size),
	}

	char_groups: Dict[str, str] = {}
	for ch in cn_chars:
		char_groups[ch] = "cn"
	for ch in jp_chars:
		char_groups[ch] = "jp"

	glyphs = build_glyphs(
		all_chars, fonts, char_groups, args.width, args.height,
		scale_factor=args.scale_factor,
		padding=args.padding,
		pixel_threshold=args.pixel_threshold,
		adaptive_percentile=args.adaptive_percentile,
		min_adaptive_threshold=args.min_adaptive_threshold,
		threshold=args.threshold,
	)

	font_name, font_symbol = derive_font_info(args.output_cpp, args.font_name)
	default_stride = math.ceil(args.width / 8)

	header_content = emit_header(font_symbol)
	cpp_content = emit_cpp(
		args.output_h.name,
		glyphs,
		font_name,
		font_symbol,
		args.width,
		args.height,
		default_stride
	)

	args.output_h.write_text(header_content, encoding="utf-8")
	args.output_cpp.write_text(cpp_content, encoding="utf-8")

	print(
		f"Generated {len(glyphs)} glyphs for font '{font_name}' "
		f"({args.width}x{args.height}) -> {args.output_cpp} / {args.output_h}"
	)
	return 0


if __name__ == "__main__":  # pragma: no cover - CLI entry point
	raise SystemExit(main())
