# M5 MorseMatrix 字体系统

> 注意：由于矩阵数量少，显示与生成效果不佳，当前字体方案已弃用，但相关代码仍可使用作参考或再开发。

## 概述

本字体系统为 M5 MorseMatrix LED 矩阵提供 UTF-8 字符渲染支持。字体数据以二维数组存储，支持高效的字形查找和渲染。

## 字体命名规则

字体文件遵循以下命名规则:
- 源文件: `m5_morsematrix_fm_<font_name>.cpp`
- 头文件: `m5_morsematrix_fm_<font_name>.h`
- 字体名称: `<font_name>` (例如 `normal_8x8`)

生成的符号:
- 字形数组: `g_m5_morsematrix_font_<font_name>_glyphs[]`
- 字形数量: `g_m5_morsematrix_font_<font_name>_count`
- 字体描述符: `g_m5_morsematrix_font_<font_name>`

## 已有字体

### normal_8x8
- **尺寸**: 8x8 像素
- **字符集**: ASCII 可打印字符 (0x20-0x7E)
- **字形数**: 95
- **字体**: Consolas

## 使用方法

### 1. 显示单个ASCII字符

```cpp
// 在 (0, 0) 位置显示字符 'A', 持续 2000ms
morsematrix.draw_char(0, 0, 'A', nullptr, 2000);

// 使用指定字体
morsematrix.draw_char(0, 0, 'B', &g_m5_morsematrix_font_normal_8x8, 1000);
```

### 2. 显示UTF-8字符串

```cpp
// 显示 ASCII 字符串
morsematrix.draw_text(0, 0, "Hello", nullptr, 3000);

// 显示数字
morsematrix.draw_text(0, 0, "2025", nullptr, 2000);

// 使用指定字体
morsematrix.draw_text(0, 0, "Hi!", &g_m5_morsematrix_font_normal_8x8, 2000);
```

### 3. 参数说明

```cpp
void draw_char(uint8_t x, uint8_t y, char ch, 
               const m5_morsematrix_font_t* font = nullptr, 
               uint32_t duration_ms = 0);

void draw_text(uint8_t x, uint8_t y, const char* utf8_str,
               const m5_morsematrix_font_t* font = nullptr,
               uint32_t duration_ms = 0);
```

- `x, y`: 起始坐标 (0-7)
- `ch` / `utf8_str`: 要显示的字符或字符串
- `font`: 字体指针，`nullptr` 使用默认字体 `normal_8x8`
- `duration_ms`: 显示持续时间(毫秒)
  - `0`: 仅设置LED状态，不自动清除
  - `> 0`: 显示指定时间后自动清除

## 生成新字体

### 工具: m5_morsematrix_fm_toolbox.py

#### 基本用法

```powershell
python m5_morsematrix_fm_toolbox.py ^
    --width 8 ^
    --height 8 ^
    --ascii-font Consolas ^
    --output-cpp m5_morsematrix_fm_normal_8x8.cpp ^
    --output-h m5_morsematrix_fm_normal_8x8.h
```

#### 命令行参数

- `--width`: 字形宽度(像素)
- `--height`: 字形高度(像素)
- `--font-size`: FreeType字体大小(默认等于height)
- `--ascii-font`: ASCII字符使用的字体(字体名称或路径)
- `--cn-font`: 中文字符使用的字体(可选)
- `--jp-font`: 日文字符使用的字体(可选)
- `--ascii-file`: ASCII字符列表文件(默认: `m5_morsematrix_fm_ascii_text.txt`)
- `--cn-file`: 中文字符列表文件(默认: `m5_morsematrix_fm_cn_text.txt`)
- `--jp-file`: 日文字符列表文件(默认: `m5_morsematrix_fm_jp_text.txt`)
- `--output-cpp`: 输出C++源文件
- `--output-h`: 输出C++头文件
- `--font-name`: 字体名称(可选，默认从文件名推导)

#### 字符列表文件格式

```
# 注释行
ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz
0123456789
!@#$%^&*()_+-=[]{}|;:'",.<>?/~`
```

- 以 `#` 开头的行为注释
- 空行会被忽略
- 每行的所有字符都会被收集
- 自动去重

#### 生成不同尺寸字体

```powershell
# 6x8 字体
python m5_morsematrix_fm_toolbox.py ^
    --width 6 --height 8 ^
    --ascii-font "Courier New" ^
    --output-cpp m5_morsematrix_fm_small_6x8.cpp ^
    --output-h m5_morsematrix_fm_small_6x8.h

# 4x6 字体
python m5_morsematrix_fm_toolbox.py ^
    --width 4 --height 6 ^
    --ascii-font "Arial" ^
    --output-cpp m5_morsematrix_fm_tiny_4x6.cpp ^
    --output-h m5_morsematrix_fm_tiny_4x6.h
```

### 集成到项目

1. 将生成的 `.cpp` 添加到 `CMakeLists.txt`:

```cmake
idf_component_register(SRCS "m5_morsematrix_component.cpp"
                            "otool_ir_nec/ir_nec_encoder.c"
                            "otool_ir_nec/ir_nec_tools.cpp"
                            "font_matrix/m5_morsematrix_fm_normal_8x8.cpp"
                            "font_matrix/m5_morsematrix_fm_small_6x8.cpp"  # 新字体
                       INCLUDE_DIRS "." 
                                    "otool_ir_nec"
                                    "otool_ir_nec/include"
                                    "font_matrix"
                        REQUIRES driver)
```

2. 在代码中使用:

```cpp
// 使用 6x8 字体
extern const m5_morsematrix_font_t g_m5_morsematrix_font_small_6x8;
morsematrix.draw_text(0, 0, "Small", &g_m5_morsematrix_font_small_6x8, 2000);
```

## 数据结构

### m5_morsematrix_glyph_t (字形)

```cpp
typedef struct {
    uint32_t codepoint;      // Unicode码点
    uint8_t width;           // 字形宽度(像素)
    uint8_t height;          // 字形高度(像素)
    uint8_t stride;          // 每行字节数 (ceil(width/8))
    const uint8_t* bitmap;   // 指向二维位图数组的指针
} m5_morsematrix_glyph_t;
```

### m5_morsematrix_font_t (字体)

```cpp
typedef struct {
    const char* name;                      // 字体名称
    uint8_t default_width;                 // 默认字形宽度
    uint8_t default_height;                // 默认字形高度
    uint8_t default_stride;                // 默认stride
    const m5_morsematrix_glyph_t* glyphs;  // 字形数组
    size_t glyph_count;                    // 字形数量
} m5_morsematrix_font_t;
```

## 位图存储格式

字形位图以**二维数组**存储:

```cpp
// 8x8 字符 'A' 的位图示例
static const uint8_t glyph_normal_8x8_0041_bitmap[8][1] = {
    { 0x00 },  // 行0: ........
    { 0x00 },  // 行1: ........
    { 0x20 },  // 行2: ..X.....
    { 0x50 },  // 行3: .X.X....
    { 0x70 },  // 行4: .XXX....
    { 0x50 },  // 行5: .X.X....
    { 0x50 },  // 行6: .X.X....
    { 0x00 }   // 行7: ........
};
```

- 每行存储为 `stride` 个字节(对8x8矩阵, stride=1)
- MSB优先: 最高位对应最左边的像素
- 1 = 像素点亮, 0 = 像素熄灭

## 性能优化

### 查找算法
- 使用**二分查找**定位字形(O(log n))
- 字形数组按Unicode码点排序

### 渲染优化
- 直接访问二维数组，无需运行时解包
- 仅渲染可见区域内的像素
- 支持零延迟模式(duration_ms=0)用于自定义刷新

## 依赖

- **Python 3.7+**
- **Pillow**: `pip install Pillow`
- **Windows字体目录**: `C:\Windows\Fonts`

## 许可

自动生成的字体文件包含注释 `DO NOT EDIT`，应由工具重新生成而非手动修改。
