#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 m5_morsematrix_fm_ascii.txt 直接生成 m5_morsematrix_fm_ascii_5x8.cpp

功能说明:
- 从 m5_morsematrix_fm_ascii.txt 读取字符的点阵数据
- 直接生成完整的 m5_morsematrix_fm_ascii_5x8.cpp 文件
- 支持 ASCII 0x20 (空格) 到 0x7E (~) 共 95 个字符

使用方法:
1. 确保 m5_morsematrix_fm_ascii.txt 存在于同一目录
2. 运行脚本: python update_font_data.py
3. 脚本会自动备份原文件并生成新的 cpp 文件

作者: GitHub Copilot
日期: 2025-10-31
"""

import re
import os
import shutil
from datetime import datetime

def parse_txt_file(txt_path):
    """解析txt文件,提取ASCII编码和对应的8字节数据"""
    data_map = {}
    
    with open(txt_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        
        # 匹配格式: /*  X[0xXXXX]    5x8  */
        # 例如: /*  ![0x0021]    5x8  */
        match = re.search(r'/\*\s+.*?\[0x([0-9A-Fa-f]{4})\]', line)
        if match:
            ascii_code_str = match.group(1)
            ascii_int = int(ascii_code_str, 16)
            
            # 下一行应该是数据行
            i += 1
            if i < len(lines):
                data_line = lines[i].strip()
                # 提取所有十六进制值
                hex_values = re.findall(r'0x([0-9A-Fa-f]{2})', data_line)
                
                if len(hex_values) >= 8:
                    # 只取前8个值
                    data_map[ascii_int] = hex_values[:8]
        
        i += 1
    
    return data_map

def generate_bitmap_definition(ascii_code, hex_values):
    """生成单个字符的bitmap定义"""
    lines = []
    lines.append(f"static const uint8_t glyph_ascii_5x8_{ascii_code:04X}_bitmap[8][1] = {{")
    for hex_val in hex_values:
        lines.append(f"    {{ 0x{hex_val.upper()} }},")
    lines.append("};")
    return "\n".join(lines)

def generate_glyph_array_entry(ascii_code):
    """生成字形数组条目"""
    return f"    {{ 0x{ascii_code:04X}, 5, 8, 1, &glyph_ascii_5x8_{ascii_code:04X}_bitmap[0][0] }},"

def generate_cpp_file(data_map, output_path):
    """生成完整的cpp文件"""
    
    print("  生成文件...")
    
    # 构建所有内容到列表中,最后一次性写入
    content_lines = []
    
    # 文件头部
    content_lines.append("// Auto-generated from m5_morsematrix_fm_ascii.txt. DO NOT EDIT.")
    content_lines.append("#include <cstddef>")
    content_lines.append('#include "m5_morsematrix_fm_ascii_5x8.h"')
    content_lines.append("")
    content_lines.append("namespace {")
    
    # 生成所有bitmap定义
    for ascii_code in range(0x20, 0x7F):
        if ascii_code in data_map:
            content_lines.append(generate_bitmap_definition(ascii_code, data_map[ascii_code]))
            content_lines.append("")
    
    # 结束 namespace
    content_lines.append("} // namespace")
    content_lines.append("")
    
    # 生成字形数组
    content_lines.append("const m5_morsematrix_glyph_t g_m5_morsematrix_font_ascii_5x8_glyphs[] = {")
    for ascii_code in range(0x20, 0x7F):
        if ascii_code in data_map:
            content_lines.append(generate_glyph_array_entry(ascii_code))
    content_lines.append("};")
    content_lines.append("")
    
    # 生成字形数量
    content_lines.append("const size_t g_m5_morsematrix_font_ascii_5x8_count =")
    content_lines.append("\tsizeof(g_m5_morsematrix_font_ascii_5x8_glyphs) / sizeof(g_m5_morsematrix_font_ascii_5x8_glyphs[0]);")
    content_lines.append("")
    
    # 生成字体结构
    content_lines.append("const m5_morsematrix_font_t g_m5_morsematrix_font_ascii_5x8 = {")
    content_lines.append('\t"ascii_5x8",')
    content_lines.append("\t5,")
    content_lines.append("\t8,")
    content_lines.append("\t1,")
    content_lines.append("\tg_m5_morsematrix_font_ascii_5x8_glyphs,")
    content_lines.append("\tg_m5_morsematrix_font_ascii_5x8_count")
    content_lines.append("};")
    
    # 一次性写入文件
    print("  写入文件...")
    with open(output_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(content_lines))
    
    print("  完成!")

def main():
    print("=" * 80)
    print(" M5 MorseMatrix 字体文件生成工具")
    print("=" * 80)
    print()
    
    # 获取脚本所在目录
    script_dir = os.path.dirname(os.path.abspath(__file__))
    
    txt_path = os.path.join(script_dir, 'm5_morsematrix_fm_ascii.txt')
    cpp_path = os.path.join(script_dir, 'm5_morsematrix_fm_ascii_5x8.cpp')
    
    if not os.path.exists(txt_path):
        print(f"[ERROR] 找不到文件: {txt_path}")
        return
    
    # 创建备份
    if os.path.exists(cpp_path):
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup_path = cpp_path + f".backup_{timestamp}"
        try:
            shutil.copy2(cpp_path, backup_path)
            print(f"[OK] 已创建备份: {os.path.basename(backup_path)}")
        except Exception as e:
            print(f"[WARNING] 无法创建备份: {e}")
    
    print()
    print("解析 txt 文件...")
    try:
        data_map = parse_txt_file(txt_path)
    except Exception as e:
        print(f"[ERROR] 解析失败: {e}")
        import traceback
        traceback.print_exc()
        return
    
    print(f"[OK] 成功解析 {len(data_map)} 个字符")
    
    if len(data_map) == 0:
        print("[ERROR] 未解析到任何数据,请检查 txt 文件格式")
        return
    
    print()
    print("生成 cpp 文件...")
    try:
        generate_cpp_file(data_map, cpp_path)
        print(f"[OK] 已生成: {os.path.basename(cpp_path)}")
    except Exception as e:
        print(f"\n[ERROR] 生成失败: {e}")
        import traceback
        traceback.print_exc()
        return
    
    print()
    print("=" * 80)
    print(" 生成完成!")
    print("=" * 80)
    print()
    print(f"输出文件: {cpp_path}")
    print(f"字符数量: {len(data_map)}")
    
    # 验证文件大小
    if os.path.exists(cpp_path):
        size = os.path.getsize(cpp_path)
        print(f"文件大小: {size:,} 字节")
    print()

if __name__ == '__main__':
    main()
