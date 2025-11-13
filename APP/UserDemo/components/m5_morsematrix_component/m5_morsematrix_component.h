/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __M5_MORSEMATRIX_COMPONENT_H__
#define __M5_MORSEMATRIX_COMPONENT_H__

// ============================================================================
// Standard Library Includes
// ============================================================================
#include <string.h>
#include <math.h>

// ============================================================================
// ESP-IDF Includes
// ============================================================================
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

// ============================================================================
// Project Includes
// ============================================================================
#include "m5_morsematrix_pinmap.h"
#include "m5_morsematrix_fm_normal_8x8.h"
#include "m5_morsematrix_fm_ascii_5x8.h"
#include "m5_morsematrix_button.h"
#include "m5_morsematrix_ir.h"
#include "m5_morsematrix_wifi.h"

// ============================================================================
// Type Definitions and Enumerations
// ============================================================================

/**
 * @brief LED映射表结构体
 * 记录每个LED的阳极和阴极GPIO引脚
 */
typedef struct {
    gpio_num_t anode_pin;    // 阳极引脚
    gpio_num_t cathode_pin;  // 阴极引脚
} led_map_t;

/**
 * @brief LED扫描模式枚举
 */
typedef enum {
    SCAN_MODE_SEQUENTIAL,      // 逐个扫描（从左到右，从上到下）
    SCAN_MODE_ROW_BY_ROW,      // 整行扫描
    SCAN_MODE_COL_BY_COL,      // 整列扫描
    SCAN_MODE_DIAGONAL_45,     // 45度斜向扫描
    SCAN_MODE_DIAGONAL_135,    // 135度斜向扫描
    SCAN_MODE_DIAGONAL_225,    // 225度斜向扫描
    SCAN_MODE_DIAGONAL_315,    // 315度斜向扫描
    SCAN_MODE_SPIRAL_OUTWARD,  // 回字由内而外扫描
    SCAN_MODE_SPIRAL_INWARD,   // 回字由外而内扫描
    SCAN_MODE_ALL_ON,          // 全部灯珠同时点亮
    SCAN_MODE_TWINKLE,         // 隔着一盏灯扫描（星星闪烁）
    SCAN_MODE_BRIGHTNESS_GRADIENT // 行亮度梯度演示
} scan_mode_t;

// ============================================================================
// Main Component Class
// ============================================================================

/**
 * @brief M5 MorseMatrix 硬件控制组件
 * 
 * 提供 8x8 LED 矩阵的完整控制功能，包括：
 * - LED 显示与刷新
 * - 字符/文本绘制与动画
 * - 摩尔斯码输入/输出
 * - 红外通信
 * - WiFi 连接
 */
class m5_morsematrix_component 
{
public:
    // ========================================================================
    // Constants - Matrix Configuration
    // ========================================================================
    static constexpr uint8_t kMatrixWidth = 8;
    static constexpr uint8_t kMatrixHeight = 8;
    static constexpr size_t kMatrixLedCount = static_cast<size_t>(kMatrixWidth) * static_cast<size_t>(kMatrixHeight);

    // ========================================================================
    // Constants - Timing Configuration
    // ========================================================================
    static constexpr uint32_t kMicrosPerSec = 1000000U;
    static constexpr uint32_t kTargetRefreshHz = 144U;
    static constexpr uint32_t kFramePeriodUs = kMicrosPerSec / kTargetRefreshHz;
    static constexpr uint32_t kTimingGuardUs = 1000U;
    static constexpr uint32_t kSlotBaseUs = (kFramePeriodUs - kTimingGuardUs) / static_cast<uint32_t>(kMatrixLedCount);
    static_assert(kFramePeriodUs > kTimingGuardUs, "Timing guard exceeds frame period");
    static constexpr UBaseType_t kRefreshTaskPriority = tskIDLE_PRIORITY + 6;

    // ========================================================================
    // Constants - Brightness Configuration
    // ========================================================================
    struct brightness_level_t {
        uint32_t on_time_us;
        uint32_t off_time_us;
    };

    static constexpr uint8_t kBrightnessLevelCount = 8;
    static const brightness_level_t kBrightnessLevels[kBrightnessLevelCount];

    /**
     * @brief 根据给定比例缩放亮度等级，用于平滑过渡效果。
     * @param brightness 原始亮度等级。
     * @param ratio 缩放比例，范围建议为 [0.0, 1.0]。
     * @return 缩放后的亮度等级。
     */
    static uint8_t scale_brightness(uint8_t brightness, float ratio);

    /**
     * @brief 获取256阶亮度梯度中指定阶的亮度时序参数（在level 1到level 7之间插值）
     * @param gradient_step 梯度阶数，范围 [0, 255]，0为最暗（关闭），255为最亮
     * @param out_on_time_us 输出参数：导通时间（微秒）
     * @param out_off_time_us 输出参数：关断时间（微秒）
     * @note 本函数在kBrightnessLevels[1]（第一个可见亮度）和kBrightnessLevels[7]（最亮）之间进行线性插值
     * @note gradient_step=0 时，on_time_us=0（LED关闭）
     * @note gradient_step=255 时，使用kBrightnessLevels[7]的参数
     */
    static void get_interpolated_brightness(uint8_t gradient_step, 
                                           uint32_t* out_on_time_us, 
                                           uint32_t* out_off_time_us);

    /**
     * @brief 将256阶梯度值转换为最接近的8阶亮度等级
     * @param gradient_step 梯度阶数，范围 [0, 255]
     * @return 对应的亮度等级 [0, 7]
     */
    static uint8_t gradient_to_brightness_level(uint8_t gradient_step);

    // ========================================================================
    // Enumerations - Animation Types
    // ========================================================================
    
    /**
     * @brief 字符动画类型
     */
    enum char_animation_t {
        CHAR_ANIM_NONE = 0,                    // 无动画，直接显示
        CHAR_ANIM_SLIDE_LEFT_IN_RIGHT_OUT,     // 左滑入+右滑出（需要前一个字符）
        CHAR_ANIM_RANDOM_FILL,                 // 随机像素填充
        CHAR_ANIM_FADE_IN                      // 逐渐亮起（淡入效果）
    };

    /**
     * @brief 文本动画类型（用于字符串整体动画效果）
     */
    enum text_animation_t {
        TEXT_ANIM_NONE = 0,              // 无动画，直接显示
        TEXT_ANIM_SLIDE_LEFT,            // 字符从右滑入，从左滑出
        TEXT_ANIM_FADE_IN_OUT,           // 梯度亮度变化：逐渐亮起 → 保持 → 逐渐熄灭
        TEXT_ANIM_STREAM_ASSEMBLE        // 流式组装
    };

    // ========================================================================
    // Constructor & Destructor
    // ========================================================================
    
    /**
     * @brief 构造函数
     */
    m5_morsematrix_component();

    /**
     * @brief 析构函数
     */
    ~m5_morsematrix_component();

    /**
     * @brief 获取全局组件单例指针
     * @return 如果单例已创建，返回指向组件的指针，否则返回nullptr
     */
    static m5_morsematrix_component* instance();

    /**
     * @brief 检查单例是否已经创建
     * @return true=存在全局单例，false=尚未构造组件
     */
    static bool has_instance();

    /**
     * @brief 获取按键模块的单例引用
     * @return m5_morsematrix_button& 单例引用
     */
    static m5_morsematrix_button& button_singleton();

    /**
     * @brief 获取按键模块的只读单例引用
     * @return const m5_morsematrix_button& 单例引用（只读）
     */
    static const m5_morsematrix_button& button_singleton_const();

    /**
     * @brief 获取红外模块的单例引用
     * @return m5_morsematrix_ir& 单例引用
     */
    static m5_morsematrix_ir& ir_singleton();

    /**
     * @brief 获取红外模块的只读单例引用
     * @return const m5_morsematrix_ir& 单例引用（只读）
     */
    static const m5_morsematrix_ir& ir_singleton_const();

    /**
     * @brief 获取 WiFi 模块的单例引用
     * @return m5_morsematrix_wifi& 单例引用
     */
    static m5_morsematrix_wifi& wifi_singleton();

    /**
     * @brief 获取 WiFi 模块的只读单例引用
     * @return const m5_morsematrix_wifi& 单例引用（只读）
     */
    static const m5_morsematrix_wifi& wifi_singleton_const();

    // ========================================================================
    // Initialization & Configuration
    // ========================================================================
    
    /**
     * @brief 初始化组件
     * @return esp_err_t 返回初始化结果
     */
    esp_err_t init();

    /**
     * @brief 初始化GPIO引脚
     * @return esp_err_t 返回初始化结果
     */
    esp_err_t gpio_init();

    /**
     * @brief 设置所有GPIO为高阻态(输入模式)
     */
    void set_all_pins_highz();

    // ========================================================================
    // Basic LED Control
    // ========================================================================
    
    /**
     * @brief 点亮指定位置的LED（使用映射表）
     * @param x X坐标 (0-7)
     * @param y Y坐标 (0-7)
     */
    void light_led(uint8_t x, uint8_t y);

    /**
     * @brief 直接点亮指定索引的LED（使用映射表索引）
     * @param index LED索引 (0-63)
     */
    void light_led_by_index(uint8_t index);

    /**
     * @brief 点亮指定范围的LED灯（索引0-63）
     * @param start_index 起始LED索引 (0-63)
     * @param end_index 结束LED索引 (0-63)
     * @param duration_ms 显示持续时间(毫秒),0表示只点亮一次不刷新
     */
    void light_leds_range(uint8_t start_index, uint8_t end_index, uint32_t duration_ms = 0);

    /**
     * @brief 熄灭所有LED
     */
    void clear_all();

    // ========================================================================
    // Brightness Control
    // ========================================================================
    
    /**
     * @brief 设置指定LED的亮度，不影响其它LED
     * @param x X坐标 (0-7)
     * @param y Y坐标 (0-7)
     * @param level 亮度等级 (0-7)
     */
    void set_pixel_brightness(uint8_t x, uint8_t y, uint8_t level);

    /**
     * @brief 使用自定义PWM时序设置指定LED的亮度（256阶梯度）
     * @param x X坐标 (0-7)
     * @param y Y坐标 (0-7)
     * @param on_time_us PWM导通时间(微秒)
     * @param off_time_us PWM关断时间(微秒)
     * @note 允许使用256阶梯度插值获得的PWM参数，实现更平滑的亮度过渡
     */
    void set_pixel_brightness_custom(uint8_t x, uint8_t y, uint32_t on_time_us, uint32_t off_time_us);

    /**
     * @brief 将整块矩阵填充为指定亮度
     * @param level 亮度等级 (0-7)
     */
    void fill_matrix(uint8_t level);

    /**
     * @brief 设置默认亮度等级，用于light_led等函数
     * @param level 亮度等级 (0-7)
     */
    void set_default_brightness_level(uint8_t level);

    /**
     * @brief 获取默认亮度等级
     * @return 当前默认亮度等级 (0-7)
     */
    uint8_t default_brightness_level() const { return default_brightness_level_; }

    /**
     * @brief 将整个行的像素亮度调整到指定的占空比
     * @param row 行索引(0-7)
     * @param on_time_us 请求的导通时间(微秒)
     * @param off_time_us 请求的关断时间(微秒)
     * @return true=调整成功, false=参数非法
     */
    bool set_row_brightness_from_us(uint8_t row, uint32_t on_time_us, uint32_t off_time_us);

    /**
     * @brief 显示行级亮度梯度(第1行最暗, 第8行最亮)
     * @param hold_duration_ms 显示持续时间(毫秒), 0表示持续显示不自动清除
     */
    void show_row_brightness_gradient(uint32_t hold_duration_ms = 0);

    // ========================================================================
    // Text & Character Drawing
    // ========================================================================
    
    /**
     * @brief 在指定位置绘制单个字符
     * @param x 起始X坐标 (0-7)
     * @param y 起始Y坐标 (0-7)
     * @param ch 要绘制的字符
     * @param font 字体指针(可选,默认使用normal_8x8)
     * @param duration_ms 显示持续时间(毫秒),0表示只设置不刷新
     * @param brightness_level 亮度等级(0-7),默认使用 default_brightness_level_
     */
    void draw_char(uint8_t x, uint8_t y, char ch, 
                   const m5_morsematrix_font_t* font = nullptr, 
                   uint32_t duration_ms = 0, 
                   uint8_t brightness_level = 0xFF);

    /**
     * @brief 在指定位置绘制单个字符（带动画效果）
     * @param x 起始X坐标 (0-7)
     * @param y 起始Y坐标 (0-7)
     * @param ch 要绘制的字符
     * @param animation 动画类型
     * @param font 字体指针(可选,默认使用normal_8x8)
     * @param duration_ms 动画完成后的显示持续时间(毫秒),0表示持续显示
     * @param animation_speed_ms 动画帧间隔(毫秒),默认50ms
     * @param brightness_level 亮度等级(0-7),默认使用 default_brightness_level_
     */
    void draw_char_with_animation(uint8_t x, uint8_t y, char ch, 
                                   char_animation_t animation,
                                   const m5_morsematrix_font_t* font = nullptr, 
                                   uint32_t duration_ms = 0,
                                   uint32_t animation_speed_ms = 50,
                                   uint8_t brightness_level = 0xFF);

    /**
     * @brief 在指定位置绘制UTF-8字符串
     * @param x 起始X坐标 (0-7)
     * @param y 起始Y坐标 (0-7)
     * @param utf8_str UTF-8编码的字符串
     * @param font 字体指针(可选,默认使用normal_8x8)
     * @param duration_ms 显示持续时间(毫秒),0表示只设置不刷新
     * @return 绘制的字符数
     */
    int draw_text(uint8_t x, uint8_t y, const char* utf8_str, 
                  const m5_morsematrix_font_t* font = nullptr, 
                  uint32_t duration_ms = 0);

    /**
     * @brief 在指定区域内绘制带动画的字符串
     * @param x1 起始X坐标 (0-7)
     * @param y1 起始Y坐标 (0-7)
     * @param x2 结束X坐标 (0-7)
     * @param y2 结束Y坐标 (0-7)
     * @param text 要显示的字符串
     * @param animation 文本动画类型
     * @param font 字体指针(可选,默认使用normal_8x8)
     * @param char_delay_ms 每个字符显示后的延迟时间(毫秒)
     * @param animation_speed_ms 动画帧间隔(毫秒),默认50ms
     * @param brightness_level 亮度等级(0-7),默认使用 default_brightness_level_
     */
    void draw_text_with_animation(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2,
                                   const char* text,
                                   text_animation_t animation,
                                   const m5_morsematrix_font_t* font = nullptr,
                                   uint32_t char_delay_ms = 0,
                                   uint32_t animation_speed_ms = 50,
                                   uint8_t brightness_level = 0xFF);

    /**
     * @brief 重置字符动画状态
     */
    void reset_char_animation_state();

    // ========================================================================
    // Text Effects & Animations
    // ========================================================================
    
    /**
     * @brief 文本轮播显示（非阻塞）
     * 每次调用检查是否需要切换到下一个字符
     * @param text 要轮播的文本
     * @param interval_ms 每个字符显示间隔（毫秒）
     * @param animation 字符动画类型
     * @param font 字体指针
     * @param brightness_level 亮度等级 (0-7)
     */
    void update_text_carousel(const char* text, uint32_t interval_ms, 
                              char_animation_t animation = CHAR_ANIM_SLIDE_LEFT_IN_RIGHT_OUT,
                              const m5_morsematrix_font_t* font = nullptr,
                              uint8_t brightness_level = 0xFF);

    /**
     * @brief 重置文本轮播状态
     */
    void reset_text_carousel();

    /**
     * @brief 第一列呼吸效果（用于等待红外数据）
     * 非阻塞函数，每次调用更新一帧
     * @param brightness_level 最大亮度等级 (0-7)
     * @return 当前应该显示的亮度等级
     */
    uint8_t update_first_column_breathing(uint8_t brightness_level = 7);

    /**
     * @brief 重置呼吸效果状态（从头开始）
     */
    void reset_breathing_effect();

    /**
     * @brief 配置刷新任务在每帧结束后让出的最小时间
     * @param min_idle_ticks 至少让出的RTOS时钟节拍数
     * @param extra_idle_us 额外让出的微秒时间
     */
    void set_frame_idle_budget(TickType_t min_idle_ticks, uint32_t extra_idle_us);

    // ========================================================================
    // Font & UTF-8 Utilities
    // ========================================================================
    
    /**
     * @brief 查找字体中的字形
     * @param font 字体指针
     * @param codepoint Unicode码点
     * @return 字形指针,未找到返回nullptr
     */
    const m5_morsematrix_glyph_t* find_glyph(const m5_morsematrix_font_t* font, uint32_t codepoint);

    /**
     * @brief 解码UTF-8字符串中的下一个字符
     * @param utf8_str UTF-8字符串指针(会被更新到下一个字符)
     * @param codepoint 输出的Unicode码点
     * @return 成功返回true,失败返回false
     */
    bool decode_utf8(const char** utf8_str, uint32_t* codepoint);

    // ========================================================================
    // Testing & Debugging
    // ========================================================================
    
    /**
     * @brief 测试函数:按指定模式扫描点亮LED
     * @param mode 扫描模式
     * @param delay_ms 每个LED或每组LED点亮的延迟时间(毫秒), 默认500ms
     */
    void test_scan_all_leds(scan_mode_t mode = SCAN_MODE_SEQUENTIAL, uint32_t delay_ms = 500);

    /**
     * @brief 调试函数:打印字形位图(用X和空格显示)
     * @param ch 要打印的字符
     * @param font 字体指针(可选,默认使用normal_8x8)
     */
    void debug_print_glyph(char ch, const m5_morsematrix_font_t* font = nullptr);

    // ========================================================================
    // Button Input
    // ========================================================================
    
    /**
     * @brief 获取按键管理单例
     * @return m5_morsematrix_button& 按键管理对象引用
     */
    m5_morsematrix_button& button() { return button_; }

    /**
     * @brief 获取按键管理单例（const版本）
     * @return const m5_morsematrix_button& 按键管理对象引用
     */
    const m5_morsematrix_button& button() const { return button_; }

    /**
     * @brief 按键被按下的处理函数（在任务上下文调用）
     * @param pin 触发按键的 GPIO 引脚
     */
    void on_button_pressed(gpio_num_t pin);

    /**
     * @brief 按键长按(>2秒)的处理函数（在任务上下文调用）
     */
    void on_button_long_pressed();

    /**
     * @brief 轮询方式检查按键状态(非中断方式)
     * @return true=按键被按下(低电平), false=按键未按下(高电平)
     */
    bool is_button_pressed();

    // ========================================================================
    // Morse Code - Display & Input
    // ========================================================================
    
    /**
     * @brief 在指定列显示当前摩斯输入的点划序列
     * @param morse_buffer 以'.'和'-'组成的字符串
     * @param column 目标列索引 (0-7)
     * @param dot_level 短按(点)的亮度等级 (0-7)
     * @param dash_level 长按(划)的亮度等级 (0-7)
     */
    void draw_morse_input_in_col(const char* morse_buffer,
                                 uint8_t column,
                                 uint8_t dot_level,
                                 uint8_t dash_level);

    /**
     * @brief 显示摩斯输入序列
     * @param morse_buffer 摩斯码字符串 (由 '.' 和 '-' 组成)
     */
    void display_morse_input(const char* morse_buffer);

    /**
     * @brief 显示解码后的文本
     * @param text 解码后的文本
     */
    void display_decoded_text(const char* text);

    /**
     * @brief 摩尔斯清除文本动画（渐隐效果）
     * @param column 目标列索引 (0-7)
     * @param max_brightness 最大亮度等级 (0-7)
     * @param fade_count 渐隐循环次数，默认3次
     */
    void draw_morse_clear_animation(uint8_t column, uint8_t max_brightness, uint8_t fade_count = 3);

    // ========================================================================
    // Morse Code - Decode & Callbacks
    // ========================================================================
    
    /**
     * @brief 解码摩斯码
     * @param morse_code 摩斯码字符串
     * @return 解码后的字符串
     */
    const char* decode_morse(const char* morse_code);

    /**
     * @brief 设置摩斯输入显示回调
     * @param callback 回调函数指针
     */
    void set_display_morse_input_callback(void (*callback)(m5_morsematrix_component* self, const char* morse_buffer));

    /**
     * @brief 设置解码文本显示回调
     * @param callback 回调函数指针
     */
    void set_display_decoded_text_callback(void (*callback)(m5_morsematrix_component* self, const char* text));

    /**
     * @brief 设置摩斯解码回调
     * @param callback 回调函数指针
     */
    void set_decode_morse_callback(const char* (*callback)(m5_morsematrix_component* self, const char* morse_code));

    /**
     * @brief 获取摩斯解码回调
     * @return 当前配置的回调函数指针
     */
    const char* (*decode_morse_callback() const)(m5_morsematrix_component* self, const char* morse_code) { 
        return decode_morse_callback_; 
    }

    /**
     * @brief 设置摩斯输入清空回调（例如长按清除文本时触发）
     * @param callback 函数指针(参数为当前按压时长，单位秒)
     */
    void set_morse_clean_text_callback(void (*callback)(uint32_t duration_s));

    /**
     * @brief 获取当前配置的清空回调
     * @return 当前配置的回调函数指针
     */
    void (*morse_clean_text_callback() const)(uint32_t duration_s) { 
        return morse_clean_text_callback_; 
    }

    /**
     * @brief 设置摩斯显示的亮度等级
     * @param dot_level 点的亮度等级 (0-7)
     * @param dash_level 划的亮度等级 (0-7)
     */
    void set_morse_display_brightness_levels(uint8_t dot_level, uint8_t dash_level);

    /**
     * @brief 获取摩斯点的亮度等级
     * @return 点的亮度等级 (0-7)
     */
    uint8_t morse_dot_brightness_level() const { return morse_dot_brightness_level_; }

    /**
     * @brief 获取摩斯划的亮度等级
     * @return 划的亮度等级 (0-7)
     */
    uint8_t morse_dash_brightness_level() const { return morse_dash_brightness_level_; }

    // ========================================================================
    // Infrared Communication
    // ========================================================================
    
    /**
     * @brief 获取红外通信管理单例
     * @return m5_morsematrix_ir& 红外通信管理对象引用
     */
    m5_morsematrix_ir& ir() { return ir_module_; }

    /**
     * @brief 获取红外通信管理单例（const版本）
     * @return const m5_morsematrix_ir& 红外通信管理对象引用
     */
    const m5_morsematrix_ir& ir() const { return ir_module_; }

    /**
     * @brief 设置 IR 接收功能（用于 LED 显示任务）
     */
    void setup_ir_for_led_show_task();

    /**
     * @brief 设置 IR 接收功能（用于 Morse 数据接收）
     * 
     * 新协议格式：
     * - sequence: 包序号 (0-254)，每个包一个字节
     * - data: 数据字节
     * - 最大支持 255 字节数据传输
     * 
     * 此函数会创建独立线程来处理 IR 数据包，不会阻塞主线程
     */
    void setup_ir_for_morse_task();

    /**
     * @brief 设置 IR 接收解码成功回调
     * 当 IR 数据接收完成并成功解码后调用
     * @param callback 函数指针，参数为：
     *                 - reception_index: 设备启动后第几次接收 (从1开始)
     *                 - decoded_text: 解码后的文本内容
     *                 - packet_count: 本次接收的有效数据包数量
     *                 - error_rate: RS 分块 parity 错误率（0.0-1.0）
     *                 - parse_success: 是否无 parity 错误且文本均为可打印字符
     */
    void set_ir_decode_success_callback(void (*callback)(uint32_t reception_index, 
                                                         const char* decoded_text, 
                                                         uint16_t packet_count, 
                                                         float error_rate, 
                                                         bool parse_success));

    /**
     * @brief 获取当前配置的 IR 解码成功回调
     * @return 当前配置的回调函数指针
     */
    void (*ir_decode_success_callback() const)(uint32_t reception_index, 
                                                const char* decoded_text, 
                                                uint16_t packet_count, 
                                                float error_rate, 
                                                bool parse_success) { 
        return ir_decode_success_callback_; 
    }

    /**
     * @brief 从 IR Morse 队列读取一个数据包（非阻塞）
     * @param sequence 输出：包序号 (0-254)
     * @param data 输出：数据字节
     * @return true=读取成功, false=队列为空
     */
    bool read_ir_morse_packet(uint8_t* sequence, uint8_t* data);

    /**
     * @brief 获取 IR Morse 队列中等待的数据包数量
     * @return 队列中的数据包数量
     */
    size_t get_ir_morse_packet_count();

    // ========================================================================
    // WiFi Connectivity
    // ========================================================================
    
    /**
     * @brief 获取 WiFi 管理单例
     * @return m5_morsematrix_wifi& WiFi 管理对象引用
     */
    m5_morsematrix_wifi& wifi() { return wifi_module_; }

    /**
     * @brief 获取 WiFi 管理单例（const版本）
     * @return const m5_morsematrix_wifi& WiFi 管理对象引用
     */
    const m5_morsematrix_wifi& wifi() const { return wifi_module_; }

    /**
     * @brief WiFi 初始化并连接到指定的AP
     * @param ssid WiFi SSID
     * @param password WiFi 密码
     * @return esp_err_t 初始化结果
     */
    esp_err_t wifi_init_and_connect(const char* ssid, const char* password);

    /**
     * @brief 获取 WiFi 连接状态
     * @return true=已连接, false=未连接
     */
    bool is_wifi_connected();

private:
    // ========================================================================
    // Private - Hardware Configuration
    // ========================================================================
    static m5_morsematrix_component* instance_;
    
    gpio_num_t row_pins[9] = {
        MORSEMATRIX_PIN_ROW0, MORSEMATRIX_PIN_ROW1, MORSEMATRIX_PIN_ROW2,
        MORSEMATRIX_PIN_ROW3, MORSEMATRIX_PIN_ROW4, MORSEMATRIX_PIN_ROW5,
        MORSEMATRIX_PIN_ROW6, MORSEMATRIX_PIN_ROW7, MORSEMATRIX_PIN_ROW8
    };
    
    gpio_num_t col_pins[8] = {
        MORSEMATRIX_PIN_COL0, MORSEMATRIX_PIN_COL1, MORSEMATRIX_PIN_COL2,
        MORSEMATRIX_PIN_COL3, MORSEMATRIX_PIN_COL4, MORSEMATRIX_PIN_COL5,
        MORSEMATRIX_PIN_COL6, MORSEMATRIX_PIN_COL7
    };

    // LED映射表: 8x8=64个LED,按物理位置 [x][y] 排列
    // 每个元素记录该位置LED的阳极和阴极引脚
    // 对角线位置没有LED(阳极和阴极短路)
    static const led_map_t led_map[8][8];

    // ========================================================================
    // Private - Display Management
    // ========================================================================
    
    SemaphoreHandle_t frame_mutex_ = nullptr;
    TaskHandle_t refresh_task_handle_ = nullptr;
    volatile bool refresh_task_running_ = false;
    uint8_t frame_buffer_[kMatrixHeight][kMatrixWidth] = {};
    uint8_t default_brightness_level_ = 7;
    
    // 自定义PWM缓冲区 - 支持任意亮度时序
    brightness_level_t custom_pwm_buffer_[kMatrixHeight][kMatrixWidth] = {};
    bool use_custom_pwm_[kMatrixHeight][kMatrixWidth] = {};

    TickType_t frame_idle_min_ticks_ = 1;
    uint32_t frame_idle_extra_us_ = 0;
    
    // GPIO 输出状态追踪
    bool row_output_state_[9] = {};
    bool col_output_state_[8] = {};
    uint32_t row_output_mask_ = 0;
    uint32_t col_output_mask_ = 0;

    // ========================================================================
    // Private - Display Core Functions
    // ========================================================================
    
    esp_err_t start_refresh_task();
    static void refresh_task(void* arg);
    
    void apply_frame(const uint8_t frame[kMatrixHeight][kMatrixWidth]);
    void apply_frame(const uint8_t frame[kMatrixHeight][kMatrixWidth], 
                     uint8_t src_x, uint8_t src_y, uint8_t src_w, uint8_t src_h,
                     uint8_t dst_x, uint8_t dst_y, uint8_t dst_w, uint8_t dst_h);
    void apply_frame_locked(const uint8_t frame[kMatrixHeight][kMatrixWidth]);
    void apply_frame_locked(const uint8_t frame[kMatrixHeight][kMatrixWidth],
                            uint8_t src_x, uint8_t src_y, uint8_t src_w, uint8_t src_h,
                            uint8_t dst_x, uint8_t dst_y, uint8_t dst_w, uint8_t dst_h);
    void set_pixel_brightness_locked(uint8_t x, uint8_t y, uint8_t level);
    uint8_t resolve_brightness_level(uint32_t on_time_us) const;
    static uint32_t compute_slot_on_time(uint8_t level);
    void mark_pin_output(gpio_num_t pin, bool is_output);

    // ========================================================================
    // Private - Animation State
    // ========================================================================
    
    // 用于动画的前一个字符缓存
    char last_char_ = '\0';
    uint8_t last_char_x_ = 0;
    uint8_t last_char_y_ = 0;
    uint8_t last_char_width_ = 0;
    uint8_t last_char_height_ = 0;
    uint8_t last_char_start_x_ = 0;
    uint8_t last_char_frame_[kMatrixHeight][kMatrixWidth] = {};
    
    // 呼吸效果状态
    uint32_t breathing_phase_ = 0;          // 呼吸相位 (0-359度)
    TickType_t last_breathing_update_ = 0;  // 上次更新时间
    
    // 文本轮播状态
    size_t carousel_char_index_ = 0;        // 当前字符索引
    TickType_t last_carousel_update_ = 0;   // 上次更新时间
    
    void play_shutdown_animation();
    void play_startup_animation();

    // ========================================================================
    // Private - Button Management
    // ========================================================================
    
    m5_morsematrix_button button_;

    // ========================================================================
    // Private - Morse Code
    // ========================================================================
    
    uint8_t morse_dot_brightness_level_ = 2;
    uint8_t morse_dash_brightness_level_ = 7;
    
    // 默认处理函数
    static const char* handle_default_decode_morse(m5_morsematrix_component* self, const char* morse_code);
    static void handle_default_morse_clean_text(uint32_t duration_s);
    static void handle_default_display_morse_input(m5_morsematrix_component* self, const char* morse_buffer);
    static void handle_default_display_decoded_text(m5_morsematrix_component* self, const char* text);
    
    // 回调函数指针
    const char* (*decode_morse_callback_)(m5_morsematrix_component*, const char*) = 
        &m5_morsematrix_component::handle_default_decode_morse;
    void (*display_morse_input_callback_)(m5_morsematrix_component*, const char*) = 
        &m5_morsematrix_component::handle_default_display_morse_input;
    void (*display_decoded_text_callback_)(m5_morsematrix_component*, const char*) = 
        &m5_morsematrix_component::handle_default_display_decoded_text;
    void (*morse_clean_text_callback_)(uint32_t duration_s) = 
        &m5_morsematrix_component::handle_default_morse_clean_text;

    // ========================================================================
    // Private - Infrared Communication
    // ========================================================================
    
    m5_morsematrix_ir ir_module_;
    
    // IR 解码成功回调
    void (*ir_decode_success_callback_)(uint32_t reception_index, 
                                        const char* decoded_text, 
                                        uint16_t packet_count, 
                                        float error_rate, 
                                        bool parse_success) = nullptr;

    // ========================================================================
    // Private - WiFi
    // ========================================================================
    
    m5_morsematrix_wifi wifi_module_;
};

#endif // __M5_MORSEMATRIX_COMPONENT_H__