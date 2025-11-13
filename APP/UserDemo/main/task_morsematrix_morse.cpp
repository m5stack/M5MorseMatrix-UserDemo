/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "task_morsematrix_morse.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>
#include <string>
#include <cctype>
#include "esp_log.h"
#include "esp_random.h"
#include "m5_morsematrix_fm_ascii_5x8.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "morse_task";

// USB串口输入配置
static char usb_input_buffer[256] = {0};
static size_t usb_input_pos = 0;
static bool usb_waiting_for_next_input = false;
static TickType_t usb_wait_start_time = 0;
static constexpr uint32_t kUsbInputWaitTimeoutMs = 5000; // 5秒等待时间

// 摩尔斯电码配置
#define MORSE_SHORT_PRESS_MAX 300      // 短按最大时长 (ms) - 点(.)
#define MORSE_LONG_PRESS_MIN 300       // 长按最小时长 (ms) - 划(-)
#define MORSE_RESET_PRESS_MIN 1500     // 重置按最小时长 (ms)
#define MORSE_CHAR_TIMEOUT 1000        // 字符间隔超时 (ms)
#define MORSE_DEBOUNCE_TIME 15         // 消抖时间 (ms)

#define MORSE_CODE_INPUT_SHOW_MODE    2 // 0:不显示 1:最右列显示当前输入符号 2:8列作为输入显示，从左往右，列点亮的长度就是按的长度

static constexpr uint8_t kMorseDotBrightnessLevel = 2;
static constexpr uint8_t kMorseDashBrightnessLevel = 7;
static constexpr size_t kMorseBufferCapacity = 256;
static constexpr uint32_t kMorseDecodeStabilizeDelayMs = 500;

static uint16_t g_press_duration_history[kMorseBufferCapacity] = {0}; // 每个摩尔符号的按压时长记录
#if MORSE_CODE_INPUT_SHOW_MODE == 2

static uint8_t compute_led_count_for_duration(uint32_t duration_ms) {
    if (duration_ms == 0) {
        return 0;
    }

    constexpr uint32_t kFirstLedHoldMs = 300;
    constexpr uint32_t kAdditionalLedStepMs = 200;

    uint32_t led_count = 1;

    if (duration_ms > kFirstLedHoldMs) {
        uint32_t remaining = duration_ms - kFirstLedHoldMs;
        led_count += static_cast<uint32_t>((remaining + kAdditionalLedStepMs - 1) / kAdditionalLedStepMs);
    }

    if (led_count > m5_morsematrix_component::kMatrixWidth) {
        led_count = m5_morsematrix_component::kMatrixWidth;
    }

    return static_cast<uint8_t>(led_count);
}

static void update_full_row_breathing(m5_morsematrix_component* matrix,
                                      const uint8_t* row_led_counts,
                                      size_t row_count,
                                      uint8_t row_offset,
                                      TickType_t& last_update,
                                      uint32_t& phase,
                                      uint8_t max_brightness) {
    if (!matrix || !row_led_counts || row_count == 0) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (last_update == 0) {
        last_update = now;
    }

    uint32_t delta_ms = pdTICKS_TO_MS(now - last_update);
    if (delta_ms == 0) {
        return;
    }

    last_update = now;

    constexpr uint32_t kBreathingPeriodMs = 1200;
    constexpr float kDegreesPerMs = 360.0f / static_cast<float>(kBreathingPeriodMs);

    float phase_increment = static_cast<float>(delta_ms) * kDegreesPerMs;
    phase = (phase + static_cast<uint32_t>(phase_increment)) % 360;

    float radians = static_cast<float>(phase) * 3.14159265f / 180.0f;
    float ratio = (sinf(radians) + 1.0f) * 0.5f;

    constexpr uint8_t kMinBreathingBrightness = 0;
    uint8_t target_brightness = kMinBreathingBrightness +
        static_cast<uint8_t>(ratio * static_cast<float>(max_brightness - kMinBreathingBrightness));

    if (target_brightness >= m5_morsematrix_component::kBrightnessLevelCount) {
        target_brightness = static_cast<uint8_t>(m5_morsematrix_component::kBrightnessLevelCount - 1);
    }

    const uint8_t max_width = static_cast<uint8_t>(m5_morsematrix_component::kMatrixWidth);

    for (size_t row_index = 0; row_index < row_count; ++row_index) {
        uint8_t led_count = row_led_counts[row_index];
        if (led_count == 0) {
            continue;
        }

        uint8_t y = static_cast<uint8_t>(row_offset + row_index);
        for (uint8_t col = 0; col < led_count && col < max_width; ++col) {
            matrix->set_pixel_brightness(col, y, target_brightness);
        }
    }
}
#endif

// 待机动画模式
enum class standby_animation_mode_t : uint8_t {
    DIAGONAL_ROLL = 0,
    CENTER_RIPPLE = 1,
};

// 全局状态（任务内部）
static struct {
    m5_morsematrix_component::text_animation_t current_display_mode = m5_morsematrix_component::TEXT_ANIM_SLIDE_LEFT;
    char ir_received_text[256] = {0};
    bool has_ir_text_to_display = false;
    TaskHandle_t ir_display_task_handle = nullptr;
    volatile bool ir_display_thread_should_stop = false;
    bool standby_active = false;
    standby_animation_mode_t standby_mode = standby_animation_mode_t::DIAGONAL_ROLL;
    TickType_t standby_last_update = 0;
    uint32_t standby_phase = 0;
    bool standby_transition_active = false;
    TickType_t standby_transition_start = 0;
    uint32_t waiting_breathing_phase = 0;
    TickType_t waiting_last_update = 0;
    bool waiting_animation_active = false;
    bool decode_waiting = false;
    TickType_t decode_wait_start = 0;
#if MORSE_CODE_INPUT_SHOW_MODE == 2
    bool full_column_breathing_active = false;
    TickType_t full_column_breathing_last_update = 0;
    uint32_t full_column_breathing_phase = 0;
    uint8_t max_filled_rows = 0;
#endif
    bool direct_text_active = false;
    bool external_input_ready = false;
    TickType_t last_ir_activity_tick = 0;
} g_morse_state;

// 当前按压预览状态（用于在释放判定前点亮列）
static struct {
    bool active = false;
    uint8_t brightness = 0;
    char symbol = '.';
    uint32_t duration_ms = 0;
} g_press_preview_state;

static size_t count_non_space_characters(const std::string& text) {
    size_t count = 0;
    for (unsigned char ch : text) {
        if (!std::isspace(ch)) {
            ++count;
            if (count > 1) {
                break;
            }
        }
    }
    return count;
}

// 解析外部输入的 JSON，有效时返回 true 并提供文本和显示模式
static bool parse_external_display_payload(const char* raw_input,
                                           std::string& out_text,
                                           m5_morsematrix_component::text_animation_t& out_mode) {
    if (!raw_input) {
        return false;
    }

    cJSON* json = cJSON_Parse(raw_input);
    if (!json) {
        return false;
    }

    if (!cJSON_IsObject(json)) {
        // ESP_LOGW(TAG, "JSON root is not an object; expecting {\"text\":..., \"style\":...}");
        cJSON_Delete(json);
        return false;
    }

    cJSON* text_field = cJSON_GetObjectItemCaseSensitive(json, "text");
    cJSON* style_field = cJSON_GetObjectItemCaseSensitive(json, "style");
    if (!cJSON_IsString(text_field) || text_field->valuestring == nullptr ||
        !cJSON_IsString(style_field) || style_field->valuestring == nullptr) {
        ESP_LOGW(TAG, "JSON must contain string fields \"text\" and \"style\"");
        cJSON_Delete(json);
        return false;
    }

    ESP_LOGI(TAG, "=== JSON Parsing Success ===");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, json) {
        if (item->string != nullptr) {
            if (cJSON_IsString(item)) {
                ESP_LOGI(TAG, "  %s: \"%s\"", item->string, item->valuestring);
            } else if (cJSON_IsNumber(item)) {
                ESP_LOGI(TAG, "  %s: %g", item->string, item->valuedouble);
            } else if (cJSON_IsBool(item)) {
                ESP_LOGI(TAG, "  %s: %s", item->string, cJSON_IsTrue(item) ? "true" : "false");
            } else {
                ESP_LOGI(TAG, "  %s: (type %d)", item->string, item->type);
            }
        }
    }
    ESP_LOGI(TAG, "===========================");

    out_text = text_field->valuestring;
    const char* style = style_field->valuestring;

    if (strcmp(style, "Stream") == 0) {
        out_mode = m5_morsematrix_component::TEXT_ANIM_STREAM_ASSEMBLE;
        ESP_LOGI(TAG, "Display mode set to: Stream");
    } else if (strcmp(style, "Fade") == 0) {
        out_mode = m5_morsematrix_component::TEXT_ANIM_FADE_IN_OUT;
        ESP_LOGI(TAG, "Display mode set to: Fade");
    } else if (strcmp(style, "Normal") == 0) {
        size_t non_space_count = count_non_space_characters(out_text);
        if (non_space_count == 1) {
            out_mode = m5_morsematrix_component::TEXT_ANIM_NONE;
            ESP_LOGI(TAG, "Display mode set to: Normal (single char -> no animation)");
        } else {
            out_mode = m5_morsematrix_component::TEXT_ANIM_SLIDE_LEFT;
            ESP_LOGI(TAG, "Display mode set to: Normal");
        }
    } else {
        ESP_LOGW(TAG, "Unknown style: %s", style);
        cJSON_Delete(json);
        return false;
    }

    cJSON_Delete(json);
    return true;
}

// 长按即将触发清屏时的右侧列呼吸效果
static void update_hold_breathing_column(m5_morsematrix_component* matrix,
                                         TickType_t& last_update,
                                         uint32_t& phase,
                                         uint8_t max_brightness) {
    (void)max_brightness; // 保持接口一致，256阶插值负责输出亮度
    
    if (!matrix) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (last_update == 0) {
        last_update = now;
    }

    uint32_t delta_ms = pdTICKS_TO_MS(now - last_update);
    if (delta_ms == 0) {
        return;
    }

    last_update = now;

    constexpr uint32_t kBreathingPeriodMs = 1000;
    constexpr float kDegreesPerMs = 360.0f / static_cast<float>(kBreathingPeriodMs);

    float phase_increment = static_cast<float>(delta_ms) * kDegreesPerMs;
    phase = (phase + static_cast<uint32_t>(phase_increment)) % 360;

    // 使用正态分布(高斯分布)曲线: exp(-x^2)
    // 将相位映射到 [-2.5, 2.5] 区间,在0点最亮,两端渐暗
    float normalized_phase = static_cast<float>(phase) / 180.0f - 1.0f; // [-1, 1]
    float gaussian_x = normalized_phase * 2.5f; // 扩展到 [-2.5, 2.5] 以获得更明显的渐变
    float gaussian_value = expf(-gaussian_x * gaussian_x); // 正态分布曲线 [0, 1]

    // 映射到256阶梯度，最低亮度使用 kBrightnessLevels[0]（完全熄灯）
    // gradient_step 范围从 0 到 255（对应 level 0 到 level 7）
    constexpr uint8_t kMinGradient = 0;   // 对应 kBrightnessLevels[0]
    constexpr uint8_t kMaxGradient = 255; // 对应 kBrightnessLevels[7]
    uint8_t gradient_step = kMinGradient + static_cast<uint8_t>(gaussian_value * static_cast<float>(kMaxGradient - kMinGradient));

    // 使用get_interpolated_brightness获取真正的256阶插值PWM参数
    uint32_t on_time_us = 0;
    uint32_t off_time_us = 0;
    m5_morsematrix_component::get_interpolated_brightness(gradient_step, &on_time_us, &off_time_us);

    // 直接使用任意PWM时序，对整列应用256阶平滑亮度过渡
    const uint8_t column = static_cast<uint8_t>(m5_morsematrix_component::kMatrixWidth - 1);
    for (uint8_t y = 0; y < m5_morsematrix_component::kMatrixHeight; ++y) {
        matrix->set_pixel_brightness_custom(column, y, on_time_us, off_time_us);
    }
}

// 停止红外文本显示线程，防止遗留后台刷新
static void stop_ir_display_thread() {
    TaskHandle_t handle = g_morse_state.ir_display_task_handle;
    if (!handle) {
        g_morse_state.ir_display_thread_should_stop = false;
        return;
    }

    g_morse_state.ir_display_thread_should_stop = true;

    constexpr int kMaxWaitIterations = 10;
    for (int i = 0; i < kMaxWaitIterations; ++i) {
        if (g_morse_state.ir_display_task_handle == nullptr) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (g_morse_state.ir_display_task_handle != nullptr) {
        vTaskDelete(handle);
        g_morse_state.ir_display_task_handle = nullptr;
    }

    g_morse_state.ir_display_thread_should_stop = false;
}

// 清空红外文本缓存并确保显示线程已关闭
static void clear_ir_display_state() {
    stop_ir_display_thread();
    g_morse_state.ir_received_text[0] = '\0';
    g_morse_state.has_ir_text_to_display = false;
    g_morse_state.direct_text_active = false;
    g_morse_state.external_input_ready = false;
    g_morse_state.last_ir_activity_tick = 0;
}

// 完整重置红外模式状态，包括等待动画
static void reset_ir_runtime_state(m5_morsematrix_component* matrix, bool clear_display) {
    clear_ir_display_state();
    g_morse_state.decode_waiting = false;
    g_morse_state.decode_wait_start = 0;

    if (matrix) {
        matrix->reset_breathing_effect();
        if (clear_display) {
            matrix->clear_all();
        }
    }
}

static standby_animation_mode_t choose_random_standby_mode() {
    uint32_t random_value = esp_random();
    return (random_value & 0x1U) ? standby_animation_mode_t::CENTER_RIPPLE
                                 : standby_animation_mode_t::DIAGONAL_ROLL;
}

// 进入待机动画
static void enter_standby_animation(m5_morsematrix_component* matrix, standby_animation_mode_t mode) {
    if (!matrix) return;

    g_morse_state.standby_mode = mode;
    g_morse_state.standby_active = true;
    g_morse_state.standby_last_update = 0;
    g_morse_state.standby_phase = 0;
    g_morse_state.standby_transition_active = true;
    g_morse_state.standby_transition_start = xTaskGetTickCount();
    g_morse_state.waiting_last_update = 0;
    g_morse_state.waiting_breathing_phase = 0;
    g_morse_state.waiting_animation_active = false;
    g_press_preview_state.active = false;
    g_press_preview_state.duration_ms = 0;
    g_morse_state.decode_waiting = false;
    g_morse_state.decode_wait_start = 0;
#if MORSE_CODE_INPUT_SHOW_MODE == 2
    g_morse_state.full_column_breathing_active = false;
    g_morse_state.full_column_breathing_last_update = 0;
    g_morse_state.full_column_breathing_phase = 0;
    g_morse_state.max_filled_rows = 0;
#endif
    g_morse_state.external_input_ready = false;
    g_morse_state.last_ir_activity_tick = 0;

    matrix->reset_breathing_effect();
    matrix->clear_all();
}

// 退出待机动画
static void exit_standby_animation(m5_morsematrix_component* matrix) {
    g_morse_state.standby_active = false;
    g_morse_state.standby_last_update = 0;
    g_morse_state.standby_phase = 0;
    g_morse_state.standby_transition_active = false;
    g_morse_state.standby_transition_start = 0;
    g_morse_state.waiting_animation_active = false;
    g_morse_state.external_input_ready = false;
    g_morse_state.last_ir_activity_tick = 0;

    if (matrix) {
        matrix->clear_all();
    }
}

// 更新待机动画帧
static void update_standby_animation(m5_morsematrix_component* matrix) {
    if (!g_morse_state.standby_active || !matrix) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    constexpr uint32_t kStandbyFrameIntervalMs = 80;
    uint32_t elapsed_ms = (g_morse_state.standby_last_update == 0)
        ? kStandbyFrameIntervalMs
        : pdTICKS_TO_MS(now - g_morse_state.standby_last_update);
    if (elapsed_ms < kStandbyFrameIntervalMs) {
        return;
    }

    g_morse_state.standby_last_update = now;

    float transition_ratio = 1.0f;
    if (g_morse_state.standby_transition_active) {
        constexpr uint32_t kStandbyTransitionDurationMs = 600;
        TickType_t start_tick = g_morse_state.standby_transition_start;
        uint32_t transition_elapsed_ms = (start_tick == 0)
            ? kStandbyTransitionDurationMs
            : pdTICKS_TO_MS(now - start_tick);

        if (transition_elapsed_ms >= kStandbyTransitionDurationMs) {
            g_morse_state.standby_transition_active = false;
            g_morse_state.standby_transition_start = 0;
            transition_ratio = 1.0f;
        } else {
            transition_ratio = static_cast<float>(transition_elapsed_ms) /
                static_cast<float>(kStandbyTransitionDurationMs);
            if (transition_ratio < 0.0f) {
                transition_ratio = 0.0f;
            }
            if (transition_ratio > 1.0f) {
                transition_ratio = 1.0f;
            }
        }
    }
#if MORSE_CODE_INPUT_SHOW_MODE == 2
    g_morse_state.full_column_breathing_active = false;
    g_morse_state.full_column_breathing_last_update = 0;
    g_morse_state.full_column_breathing_phase = 0;
    g_morse_state.max_filled_rows = 0;
#endif

    const uint8_t width = static_cast<uint8_t>(m5_morsematrix_component::kMatrixWidth);
    const uint8_t height = static_cast<uint8_t>(m5_morsematrix_component::kMatrixHeight);

    matrix->clear_all();

    switch (g_morse_state.standby_mode) {
        case standby_animation_mode_t::DIAGONAL_ROLL: {
            const uint32_t cycle_steps = static_cast<uint32_t>(width + height);
            if (cycle_steps == 0) {
                break;
            }

            const float cycle = static_cast<float>(cycle_steps);
            const float phase = static_cast<float>(g_morse_state.standby_phase);
            constexpr float kTwoPi = 6.2831853f;
            const float frequency = kTwoPi / cycle;
            const float gaussian_min = expf(-2.25f); // 1.5^2
            const float gaussian_range = 1.0f - gaussian_min;

            for (uint8_t y = 0; y < height; ++y) {
                for (uint8_t x = 0; x < width; ++x) {
                    // 计算对角线位置相对于相位的偏移
                    float diag = static_cast<float>(x + y);
                    float angle = (diag - phase) * frequency;
                    float cosine_value = cosf(angle); // [-1, 1]
                    
                    // 使用正态分布：将 cosine 映射到高斯函数
                    // cosine_value [-1, 1] → gaussian_x [-1.5, 1.5] (降低峰值，使亮度更均衡)
                    float gaussian_x = cosine_value * 1.5f;
                    float gaussian_value = expf(-gaussian_x * gaussian_x); // [0.105, 1]

                    float normalized_gaussian = 0.0f;
                    if (gaussian_range > 0.0f) {
                        normalized_gaussian = (gaussian_value - gaussian_min) / gaussian_range;
                    }
                    if (normalized_gaussian < 0.0f) {
                        normalized_gaussian = 0.0f;
                    } else if (normalized_gaussian > 1.0f) {
                        normalized_gaussian = 1.0f;
                    }
                    
                    // 映射到 256 阶梯度，最低亮度保持为 level 0（熄灯）
                    constexpr uint8_t kMinGradient = 0;
                    constexpr uint8_t kMaxGradient = 255;
                    uint8_t gradient_step = kMinGradient + static_cast<uint8_t>(normalized_gaussian * static_cast<float>(kMaxGradient - kMinGradient));
                    
                    // 获取插值后的 PWM 参数
                    uint32_t on_time_us = 0;
                    uint32_t off_time_us = 0;
                    m5_morsematrix_component::get_interpolated_brightness(gradient_step, &on_time_us, &off_time_us);
                    
                    // 应用过渡效果
                    if (transition_ratio < 1.0f) {
                        on_time_us = static_cast<uint32_t>(static_cast<float>(on_time_us) * transition_ratio);
                    }
                    
                    matrix->set_pixel_brightness_custom(x, y, on_time_us, off_time_us);
                }
            }

            g_morse_state.standby_phase = (g_morse_state.standby_phase + 1) % cycle_steps;
            break;
        }

        case standby_animation_mode_t::CENTER_RIPPLE: {
            constexpr float kPhaseStep = 0.35f;
            const float phase = static_cast<float>(g_morse_state.standby_phase) * kPhaseStep;
            const float center_x = (static_cast<float>(width) - 1.0f) * 0.5f;
            const float center_y = (static_cast<float>(height) - 1.0f) * 0.5f;
            const float gaussian_min = expf(-2.25f); // 1.5^2
            const float gaussian_range = 1.0f - gaussian_min;

            for (uint8_t y = 0; y < height; ++y) {
                for (uint8_t x = 0; x < width; ++x) {
                    // 计算距离中心的距离
                    float dx = static_cast<float>(x) - center_x;
                    float dy = static_cast<float>(y) - center_y;
                    float distance = sqrtf(dx * dx + dy * dy);
                    
                    // 计算波纹效果
                    float wave = sinf(distance - phase); // [-1, 1]
                    
                    // 使用正态分布：将 wave 映射到高斯函数
                    // wave [-1, 1] → gaussian_x [-1.5, 1.5] (降低峰值，使亮度更均衡)
                    float gaussian_x = wave * 1.5f;
                    float gaussian_value = expf(-gaussian_x * gaussian_x); // [0.105, 1]

                    float normalized_gaussian = 0.0f;
                    if (gaussian_range > 0.0f) {
                        normalized_gaussian = (gaussian_value - gaussian_min) / gaussian_range;
                    }
                    if (normalized_gaussian < 0.0f) {
                        normalized_gaussian = 0.0f;
                    } else if (normalized_gaussian > 1.0f) {
                        normalized_gaussian = 1.0f;
                    }
                    
                    // 映射到 256 阶梯度，最低亮度保持为 level 0（熄灯）
                    constexpr uint8_t kMinGradient = 0;
                    constexpr uint8_t kMaxGradient = 255;
                    uint8_t gradient_step = kMinGradient + static_cast<uint8_t>(normalized_gaussian * static_cast<float>(kMaxGradient - kMinGradient));
                    
                    // 获取插值后的 PWM 参数
                    uint32_t on_time_us = 0;
                    uint32_t off_time_us = 0;
                    m5_morsematrix_component::get_interpolated_brightness(gradient_step, &on_time_us, &off_time_us);
                    
                    // 应用过渡效果
                    if (transition_ratio < 1.0f) {
                        on_time_us = static_cast<uint32_t>(static_cast<float>(on_time_us) * transition_ratio);
                    }
                    
                    matrix->set_pixel_brightness_custom(x, y, on_time_us, off_time_us);
                }
            }

            g_morse_state.standby_phase = (g_morse_state.standby_phase + 1) % 360;
            break;
        }
    }
}

// 摩尔斯电码表
static const char* MORSE_TABLE[] = {
    ".-",   "A", "-...", "B", "-.-.", "C", "-..",  "D", ".",    "E",
    "..-.", "F", "--.",  "G", "....", "H", "..",   "I", ".---", "J",
    "-.-",  "K", ".-..", "L", "--",   "M", "-.",   "N", "---",  "O",
    ".--.", "P", "--.-", "Q", ".-.",  "R", "...",  "S", "-",    "T",
    "..-",  "U", "...-", "V", ".--",  "W", "-..-", "X", "-.--", "Y",
    "--..", "Z",
    "-----", "0", ".----", "1", "..---", "2", "...--", "3", "....-", "4",
    ".....", "5", "-....", "6", "--...", "7", "---..", "8", "----.", "9"
};
static const int MORSE_TABLE_SIZE = sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]);

// 前向声明
static m5_morsematrix_component* g_matrix_ptr = nullptr;

// 摩尔斯电码解码函数
static const char* decode_morse_handler(m5_morsematrix_component* matrix, const char* morse_code) {
    (void)matrix;
    for (int i = 0; i < MORSE_TABLE_SIZE; i += 2) {
        if (strcmp(morse_code, MORSE_TABLE[i]) == 0) {
            return MORSE_TABLE[i + 1];
        }
    }
    return "?";
}

// 显示摩尔斯输入
static void display_morse_input_handler(m5_morsematrix_component* matrix, const char* morse_buffer) {
    if (!matrix) return;
    if (!morse_buffer) morse_buffer = "";

#if MORSE_CODE_INPUT_SHOW_MODE == 0
    (void)morse_buffer;
    return;
#elif MORSE_CODE_INPUT_SHOW_MODE == 1
    const uint8_t target_column = static_cast<uint8_t>(m5_morsematrix_component::kMatrixWidth - 1);
    const uint8_t max_rows = static_cast<uint8_t>(m5_morsematrix_component::kMatrixHeight);

    size_t len = strlen(morse_buffer);

    matrix->draw_morse_input_in_col(morse_buffer,
                                     target_column,
                                     matrix->morse_dot_brightness_level(),
                                     matrix->morse_dash_brightness_level());

    if (g_press_preview_state.active) {
        uint8_t preview_row = (len < max_rows) ? static_cast<uint8_t>(len)
                                               : static_cast<uint8_t>(max_rows - 1);
        uint8_t brightness = g_press_preview_state.brightness;
        if (brightness >= m5_morsematrix_component::kBrightnessLevelCount) {
            brightness = static_cast<uint8_t>(m5_morsematrix_component::kBrightnessLevelCount - 1);
        }
        matrix->set_pixel_brightness(target_column, preview_row, brightness);
    }
#elif MORSE_CODE_INPUT_SHOW_MODE == 2
    // 逆时针旋转90度：从下往上显示行，每行从左往右点亮
    const uint8_t width = static_cast<uint8_t>(m5_morsematrix_component::kMatrixWidth);
    const uint8_t height = static_cast<uint8_t>(m5_morsematrix_component::kMatrixHeight);

    matrix->clear_all();

    size_t len = strlen(morse_buffer);
    bool has_preview = g_press_preview_state.active;
    size_t total_slots = len + (has_preview ? 1 : 0);

    if (total_slots == 0) {
        return;
    }

    // 从上往下显示，最多显示8行
    size_t start_slot = (total_slots > height) ? (total_slots - height) : 0;
    size_t end_slot = start_slot + ((total_slots - start_slot > height) ? height : (total_slots - start_slot));
    size_t row_count = end_slot - start_slot;
    if (row_count == 0) {
        return;
    }

    uint8_t row_led_counts[m5_morsematrix_component::kMatrixHeight] = {}; // 映射当前可见行对应的亮灯数量
    bool breathing_active = g_morse_state.full_column_breathing_active;
    uint8_t max_led_count = 0;

    // 第一步：计算每行应该显示的LED数量
    for (size_t slot = start_slot; slot < end_slot; ++slot) {
        bool is_preview = has_preview && (slot == len);
        // 从上往下：row = slot - start_slot
        uint8_t row = static_cast<uint8_t>(slot - start_slot);

        uint32_t duration_ms = 0;

        if (is_preview) {
            duration_ms = g_press_preview_state.duration_ms;
        } else if (slot < len && slot < kMorseBufferCapacity) {
            duration_ms = g_press_duration_history[slot];
        }

        uint8_t led_count = compute_led_count_for_duration(duration_ms);
        if (is_preview && led_count == 0) {
            led_count = 1;
        }

        if (led_count > width) {
            led_count = width;
        }

        bool is_last_committed_symbol = (!has_preview && len > 0 && slot == (len - 1));
        if (is_last_committed_symbol) {
            // 最后一个已提交符号：如果超过2个LED，回退到2个；否则保持原样
            if (led_count > 2) {
                led_count = 2;
            }
        }

        row_led_counts[row] = led_count;
        if (led_count > max_led_count) {
            max_led_count = led_count;
        }
    }

    // 第二步：如果有多行，将非最后一行超过2个LED的回退到2个
    if (row_count > 1) {
        // 防止历史输入占满屏幕：只有最后一行允许超过两列亮灯
        for (size_t i = 0; i < row_count - 1; ++i) {
            if (row_led_counts[i] > 2) {
                row_led_counts[i] = 2;
            }
        }
    }

    // 第三步：根据计算结果绘制所有行
    if (!breathing_active) {
        uint8_t brightness = matrix->morse_dash_brightness_level();
        if (brightness >= m5_morsematrix_component::kBrightnessLevelCount) {
            brightness = static_cast<uint8_t>(m5_morsematrix_component::kBrightnessLevelCount - 1);
        }

        for (uint8_t row = 0; row < row_count; ++row) {
            uint8_t led_count = row_led_counts[row];
            // 每行从左往右点亮
            for (uint8_t x = 0; x < led_count; ++x) {
                matrix->set_pixel_brightness(x, row, brightness);
            }
        }
    }

    // 当任何一行填满整行时，激活呼吸效果
    if (!breathing_active && max_led_count >= width) {
        breathing_active = true;
        g_morse_state.full_column_breathing_active = true;
        g_morse_state.full_column_breathing_last_update = 0;
        g_morse_state.full_column_breathing_phase = 0;
        g_morse_state.max_filled_rows = max_led_count;
    } else if (breathing_active) {
        g_morse_state.max_filled_rows = max_led_count;
    }

    if (breathing_active) {
        // 从上往下显示，起始行为0
        update_full_row_breathing(matrix,
                                  row_led_counts,
                                  row_count,
                                  0,
                                  g_morse_state.full_column_breathing_last_update,
                                  g_morse_state.full_column_breathing_phase,
                                  matrix->morse_dash_brightness_level());
    }
#else
    const uint8_t target_column = static_cast<uint8_t>(m5_morsematrix_component::kMatrixWidth - 1);
    const uint8_t max_rows = static_cast<uint8_t>(m5_morsematrix_component::kMatrixHeight);

    size_t len = strlen(morse_buffer);

    matrix->draw_morse_input_in_col(morse_buffer,
                                     target_column,
                                     matrix->morse_dot_brightness_level(),
                                     matrix->morse_dash_brightness_level());

    if (g_press_preview_state.active) {
        uint8_t preview_row = (len < max_rows) ? static_cast<uint8_t>(len)
                                               : static_cast<uint8_t>(max_rows - 1);
        uint8_t brightness = g_press_preview_state.brightness;
        if (brightness >= m5_morsematrix_component::kBrightnessLevelCount) {
            brightness = static_cast<uint8_t>(m5_morsematrix_component::kBrightnessLevelCount - 1);
        }
        matrix->set_pixel_brightness(target_column, preview_row, brightness);
    }
#endif
}

// 显示已解码文本
static void display_decoded_text_handler(m5_morsematrix_component* matrix, const char* text) {
    if (!matrix) return;
    
    if (text == nullptr || text[0] == '\0') {
        matrix->clear_all();
        return;
    }
    
    const size_t len = strlen(text);
    const char latest = text[len - 1];
    
    matrix->clear_all();
    
#if MORSE_CODE_INPUT_SHOW_MODE == 1
    // MODE1: 使用滑入滑出动画
    matrix->draw_char_with_animation(1, 0, latest,
                                     m5_morsematrix_component::CHAR_ANIM_SLIDE_LEFT_IN_RIGHT_OUT,
                                     &g_m5_morsematrix_font_ascii_5x8,
                                     0, 50,
                                     matrix->morse_dash_brightness_level());
#elif MORSE_CODE_INPUT_SHOW_MODE == 2
    // MODE2: 无动画直接显示
    matrix->draw_char_with_animation(1, 0, latest,
                                     m5_morsematrix_component::CHAR_ANIM_FADE_IN,
                                     &g_m5_morsematrix_font_ascii_5x8,
                                     0, 0,
                                     matrix->morse_dash_brightness_level());
#else
    // 默认模式: 使用滑入滑出动画
    matrix->draw_char_with_animation(1, 0, latest,
                                     m5_morsematrix_component::CHAR_ANIM_SLIDE_LEFT_IN_RIGHT_OUT,
                                     &g_m5_morsematrix_font_ascii_5x8,
                                     0, 50,
                                     matrix->morse_dash_brightness_level());
#endif
}

// 清除文本处理
static void morse_clean_text_handler(uint32_t duration_s) {
    ESP_LOGI(TAG, "Morse clean text handler: %lus", static_cast<unsigned long>(duration_s));
    
    if (g_matrix_ptr) {
        g_matrix_ptr->reset_char_animation_state();
        g_matrix_ptr->clear_all();
    }
}

// IR解码成功回调
static void ir_decode_success_handler(uint32_t reception_index,
                                      const char* decoded_text,
                                      uint16_t packet_count,
                                      float error_rate,
                                      bool parse_success) {
    // 只允许在等待输入状态时进行解码
    if (!g_morse_state.external_input_ready) {
        ESP_LOGW(TAG, "IR decode ignored - not in waiting input state");
        return;
    }
    
    ESP_LOGI(TAG, "=== IR Decode Success ===");
    ESP_LOGI(TAG, "Reception #%lu", static_cast<unsigned long>(reception_index));
    ESP_LOGI(TAG, "Packets received: %u", static_cast<unsigned>(packet_count));
    ESP_LOGI(TAG, "Decoded text: %s", decoded_text);
    ESP_LOGI(TAG, "Chunk error rate: %.1f%%", error_rate * 100.0f);
    ESP_LOGI(TAG, "Integrity check: %s", parse_success ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "========================");
    
    std::string display_text = decoded_text ? decoded_text : "";
    m5_morsematrix_component::text_animation_t display_mode = m5_morsematrix_component::TEXT_ANIM_SLIDE_LEFT;

    if (parse_external_display_payload(decoded_text, display_text, display_mode)) {
        g_morse_state.current_display_mode = display_mode;
    } else {
        if (decoded_text && decoded_text[0] != '\0') {
            ESP_LOGI(TAG, "JSON parsing failed or missing required fields, displaying raw text with Normal mode");
        } else {
            ESP_LOGI(TAG, "JSON parsing failed or missing required fields, no text provided");
        }
        g_morse_state.current_display_mode = m5_morsematrix_component::TEXT_ANIM_SLIDE_LEFT;
    }
    
    // 限制文本长度最多250个字符，防止越界
    size_t text_len = display_text.size();
    if (text_len > 250) {
        ESP_LOGW(TAG, "IR text too long (%zu chars), truncating to 250", text_len);
        strncpy(g_morse_state.ir_received_text, display_text.c_str(), 250);
        g_morse_state.ir_received_text[250] = '\0';
    } else {
        strncpy(g_morse_state.ir_received_text, display_text.c_str(), sizeof(g_morse_state.ir_received_text) - 1);
        g_morse_state.ir_received_text[sizeof(g_morse_state.ir_received_text) - 1] = '\0';
    }
    g_morse_state.direct_text_active = false;
    g_morse_state.has_ir_text_to_display = true;
    g_morse_state.external_input_ready = false;
    g_morse_state.last_ir_activity_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "IR Receive Mode: Text saved for display");
}

// IR文本显示线程
static void ir_text_display_task(void* param) {
    (void)param;
    
    ESP_LOGI(TAG, "IR text display thread started");
    
    // 只在开始时清屏一次
    if (g_matrix_ptr) {
        g_matrix_ptr->clear_all();
    }
    
    while (!g_morse_state.ir_display_thread_should_stop && g_matrix_ptr) {
        
        switch (g_morse_state.current_display_mode) {
            case m5_morsematrix_component::TEXT_ANIM_SLIDE_LEFT:
                g_matrix_ptr->draw_text_with_animation(
                    0, 0, 7, 7,
                    g_morse_state.ir_received_text,
                    m5_morsematrix_component::TEXT_ANIM_SLIDE_LEFT,
                    &g_m5_morsematrix_font_ascii_5x8,
                    500, 150, kMorseDashBrightnessLevel);
                break;
                
            case m5_morsematrix_component::TEXT_ANIM_FADE_IN_OUT:
                g_matrix_ptr->draw_text_with_animation(
                    0, 0, 7, 7,
                    g_morse_state.ir_received_text,
                    m5_morsematrix_component::TEXT_ANIM_FADE_IN_OUT,
                    &g_m5_morsematrix_font_ascii_5x8,
                    500, 80, kMorseDashBrightnessLevel);
                break;
                
            case m5_morsematrix_component::TEXT_ANIM_STREAM_ASSEMBLE:
                g_matrix_ptr->draw_text_with_animation(
                    0, 0, 7, 7,
                    g_morse_state.ir_received_text,
                    m5_morsematrix_component::TEXT_ANIM_STREAM_ASSEMBLE,
                    &g_m5_morsematrix_font_ascii_5x8,
                    100, 80, kMorseDashBrightnessLevel);
                break;
                
            default:
                g_matrix_ptr->draw_text_with_animation(
                    0, 0, 7, 7,
                    g_morse_state.ir_received_text,
                    m5_morsematrix_component::TEXT_ANIM_NONE,
                    &g_m5_morsematrix_font_ascii_5x8,
                    800, 0, kMorseDashBrightnessLevel);
                break;
        }
        
        // 动画完成后短暂延迟再循环
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    ESP_LOGI(TAG, "IR text display thread stopped");
    if (g_matrix_ptr) {
        g_matrix_ptr->clear_all();
    }
    
    g_morse_state.ir_display_task_handle = nullptr;
    g_morse_state.ir_display_thread_should_stop = false;
    vTaskDelete(NULL);
}

// USB串口输入处理函数（复用红外显示逻辑）
static void handle_usb_input(const char* input_text) {
    if (!input_text || input_text[0] == '\0') {
        return;
    }
    
    ESP_LOGI(TAG, "=== USB Serial Input Received ===");
    ESP_LOGI(TAG, "Input text: %s", input_text);
    ESP_LOGI(TAG, "=================================");
    
    // 移除 external_input_ready 检查，允许在任何时候接收新的USB输入
    // 无论是在等待界面还是显示界面，都接受新输入

    g_morse_state.external_input_ready = false;
    g_morse_state.direct_text_active = false;
    std::string display_text = input_text;
    m5_morsematrix_component::text_animation_t display_mode = m5_morsematrix_component::TEXT_ANIM_SLIDE_LEFT;

    if (parse_external_display_payload(input_text, display_text, display_mode)) {
        g_morse_state.current_display_mode = display_mode;
    } else {
        // JSON解析失败，检查是否为单字符输入
        size_t non_space_count = 0;
        char single_char = '\0';
        for (const char* p = input_text; *p != '\0'; ++p) {
            if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                non_space_count++;
                if (non_space_count == 1) {
                    single_char = *p;
                }
                if (non_space_count > 1) {
                    break;
                }
            }
        }

        if (non_space_count == 1) {
            ESP_LOGI(TAG, "JSON parsing failed or missing required fields, single character detected: '%c'", single_char);
            ESP_LOGI(TAG, "Using TEXT_ANIM_NONE via draw_text_with_animation for immediate display");

            if (g_matrix_ptr) {
                stop_ir_display_thread();

                g_morse_state.ir_received_text[0] = '\0';
                g_morse_state.has_ir_text_to_display = false;

                g_matrix_ptr->reset_breathing_effect();
                g_matrix_ptr->clear_all();

                char text_buffer[2] = {single_char, '\0'};
                g_morse_state.current_display_mode = m5_morsematrix_component::TEXT_ANIM_NONE;
                g_morse_state.direct_text_active = true;
                g_morse_state.waiting_animation_active = false;
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
                g_morse_state.external_input_ready = false;
                g_matrix_ptr->draw_text_with_animation(
                    1, 0, 7, 7,
                    text_buffer,
                    m5_morsematrix_component::TEXT_ANIM_NONE,
                    &g_m5_morsematrix_font_ascii_5x8,
                    0,
                    0,
                    kMorseDashBrightnessLevel);
            }
            return;
        } else {
            ESP_LOGI(TAG, "JSON parsing failed or missing required fields, displaying raw text with Normal mode");
            display_text = input_text;
            g_morse_state.current_display_mode = m5_morsematrix_component::TEXT_ANIM_SLIDE_LEFT;
        }
    }
    
    // 保存文本用于显示，限制最多250个字符防止越界
    size_t text_len = display_text.size();
    if (text_len > 250) {
        ESP_LOGW(TAG, "USB input too long (%zu chars), truncating to 250", text_len);
        strncpy(g_morse_state.ir_received_text, display_text.c_str(), 250);
        g_morse_state.ir_received_text[250] = '\0';
    } else {
        strncpy(g_morse_state.ir_received_text, display_text.c_str(), sizeof(g_morse_state.ir_received_text) - 1);
        g_morse_state.ir_received_text[sizeof(g_morse_state.ir_received_text) - 1] = '\0';
    }
    g_morse_state.has_ir_text_to_display = true;
    ESP_LOGI(TAG, "USB Serial Input: Text saved for display");
}

// 显示输入提示符
static void show_input_prompt() {
    const char* prompt = "\r\nInput: ";
    usb_serial_jtag_write_bytes(prompt, strlen(prompt), portMAX_DELAY);
    usb_waiting_for_next_input = false;
    usb_wait_start_time = 0;
}

// 读取USB串口输入（非阻塞）；一旦遇到行结束符立即交给解析逻辑
static void process_usb_input() {
    // 检查是否在等待状态中
    if (usb_waiting_for_next_input) {
        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed_ms = pdTICKS_TO_MS(now - usb_wait_start_time);
        
        // 超过5秒，自动显示提示符
        if (elapsed_ms >= kUsbInputWaitTimeoutMs) {
            show_input_prompt();
        }
    }
    
    uint8_t data[128];
    int len = usb_serial_jtag_read_bytes(data, sizeof(data) - 1, 0);
    
    if (len > 0) {
        for (int i = 0; i < len; i++) {
            char c = (char)data[i];
            
            // 处理换行符或回车符，作为输入结束标志
            if (c == '\n' || c == '\r') {
                // 如果正在等待状态，有新输入就提前显示提示符
                if (usb_waiting_for_next_input) {
                    show_input_prompt();
                    continue; // 继续处理后续字符
                }
                
                // 空输入，直接重新显示提示符
                if (usb_input_pos == 0) {
                    show_input_prompt();
                    continue;
                }
                
                // 有内容的输入
                usb_input_buffer[usb_input_pos] = '\0';
                
                // 发送换行，让输出更整洁
                const char* newline = "\r\n";
                usb_serial_jtag_write_bytes(newline, 2, portMAX_DELAY);
                
                // 处理输入
                handle_usb_input(usb_input_buffer);
                usb_input_pos = 0;
                
                // 进入等待状态
                usb_waiting_for_next_input = true;
                usb_wait_start_time = xTaskGetTickCount();
            }
            // 累积字符到缓冲区，并实时回显
            else if (usb_input_pos < sizeof(usb_input_buffer) - 1) {
                // 如果是在等待状态收到字符，提前显示提示符
                if (usb_waiting_for_next_input) {
                    show_input_prompt();
                }
                
                usb_input_buffer[usb_input_pos++] = c;
                
                // 实时回显字符
                usb_serial_jtag_write_bytes((const uint8_t*)&c, 1, portMAX_DELAY);
            }
            // 缓冲区满，强制处理
            else {
                usb_input_buffer[usb_input_pos] = '\0';
                
                // 发送换行
                const char* newline = "\r\n";
                usb_serial_jtag_write_bytes(newline, 2, portMAX_DELAY);
                
                handle_usb_input(usb_input_buffer);
                usb_input_pos = 0;
                
                // 进入等待状态
                usb_waiting_for_next_input = true;
                usb_wait_start_time = xTaskGetTickCount();
            }
        }
    }
}

// 显示红外接收文本
static void display_ir_received_text() {
    if (g_morse_state.ir_received_text[0] == '\0') {
        ESP_LOGW(TAG, "No IR text to display");
        return;
    }
    
    if (g_morse_state.ir_display_task_handle != nullptr) {
        ESP_LOGW(TAG, "IR text display thread already running");
        return;
    }
    
    BaseType_t result = xTaskCreate(
        ir_text_display_task,
        "ir_display",
        4096,
        NULL,
        0,  // 优先级 0 - IR文本显示（最低优先级后台任务）
        &g_morse_state.ir_display_task_handle);
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IR text display thread");
        g_morse_state.ir_display_task_handle = nullptr;
    } else {
        ESP_LOGI(TAG, "IR text display thread created");
    }
}

// 主任务函数
void task_morsematrix_morse_run(m5_morsematrix_component* morsematrix) {
    if (!morsematrix) {
        ESP_LOGE(TAG, "Invalid morsematrix pointer");
        return;
    }
    
    g_matrix_ptr = morsematrix;
    
    ESP_LOGI(TAG, "Starting Morse Code Input Mode...");
    
    // 设置回调
    morsematrix->set_morse_clean_text_callback(&morse_clean_text_handler);
    morsematrix->set_decode_morse_callback(&decode_morse_handler);
    morsematrix->set_morse_display_brightness_levels(kMorseDotBrightnessLevel, kMorseDashBrightnessLevel);
    morsematrix->set_display_morse_input_callback(&display_morse_input_handler);
    morsematrix->set_display_decoded_text_callback(&display_decoded_text_handler);
    morsematrix->set_ir_decode_success_callback(&ir_decode_success_handler);
    
    // 配置按键
    auto& morse_button = m5_morsematrix_component::button_singleton();
    morse_button.reset();
    morse_button.set_debounce(MORSE_DEBOUNCE_TIME);
    morse_button.set_thresholds(MORSE_SHORT_PRESS_MAX, MORSE_LONG_PRESS_MIN, MORSE_RESET_PRESS_MIN);
    
    // 摩尔斯输入缓冲区
    char morse_buffer[kMorseBufferCapacity] = {0};
    char decoded_text[256] = {0};
    TickType_t last_input_time = 0;
    bool need_decode = false;
    bool screen_is_empty = false;
    TickType_t last_user_interaction = xTaskGetTickCount();
    TickType_t hold_breathing_last_update = 0;
    uint32_t hold_breathing_phase = 0;
    bool press_started_in_standby = false;
    
    ESP_LOGI(TAG, "Morse Code Input Ready!");
    ESP_LOGI(TAG, "Short press (<300ms): dot(.)");
    ESP_LOGI(TAG, "Long press (>=300ms): dash(-)");
    ESP_LOGI(TAG, "Reset press (>=1500ms): clear all");
    
    // 显示USB串口输入提示符
    show_input_prompt();
    
    morsematrix->reset_breathing_effect();
    g_morse_state.standby_mode = choose_random_standby_mode();
    enter_standby_animation(morsematrix, g_morse_state.standby_mode);
    
    while (true) {
        TickType_t now = xTaskGetTickCount();
        bool raw_state = morsematrix->is_button_pressed();

        if (g_morse_state.external_input_ready) {
            size_t pending_ir_packets = morsematrix->get_ir_morse_packet_count();
            if (pending_ir_packets > 0) {
                g_morse_state.last_ir_activity_tick = now;
            }
        }

        update_standby_animation(morsematrix);
        
        if (!morse_button.update(raw_state)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        
        m5_morsematrix_button::event_type_t event = morse_button.get_event();

        // 记录事件（排除高频的 PRESS_TICK 以提高性能）
        if (event != m5_morsematrix_button::EVENT_NONE && event != m5_morsematrix_button::EVENT_PRESS_TICK) {
            const char* event_names[] = {"NONE", "PRESSED", "PRESS_TICK", "RELEASED", "SHORT_PRESS", "LONG_PRESS", "VERY_LONG_PRESS"};
            if (event < 7) {
                ESP_LOGI(TAG, "==> Event: %s (button_state=%d)", event_names[event], morse_button.get_state());
            }
        }

        if (event == m5_morsematrix_button::EVENT_PRESSED) {
            last_user_interaction = now;
            hold_breathing_last_update = 0;
            hold_breathing_phase = 0;
            g_morse_state.decode_waiting = false;
            g_morse_state.decode_wait_start = 0;

            // 检查是否正在显示IR/USB文本
            bool was_displaying_text = (g_morse_state.ir_display_task_handle != nullptr) || g_morse_state.direct_text_active;
            
            bool was_standby = g_morse_state.standby_active;
            if (was_standby) {
                exit_standby_animation(morsematrix);
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
            }
            
            // 如果正在显示文本，按键操作应停止显示并清理
            if (was_displaying_text) {
                ESP_LOGI(TAG, "Key pressed while displaying text - stopping display");
                
                // 停止文本显示线程
                stop_ir_display_thread();
                
                // 清空红外接收文本
                g_morse_state.ir_received_text[0] = '\0';
                g_morse_state.has_ir_text_to_display = false;
                g_morse_state.direct_text_active = false;
                g_morse_state.external_input_ready = false;
                
                // 清空已解码文本
                decoded_text[0] = '\0';
                
                // 清空摩尔码输入缓冲
                morse_buffer[0] = '\0';
                memset(g_press_duration_history, 0, sizeof(g_press_duration_history));
                need_decode = false;
                
                // 重置呼吸效果
                morsematrix->reset_breathing_effect();
                
                // 清屏
                morsematrix->clear_all();
                
                // 标记为空屏，准备进入摩尔码输入
                screen_is_empty = true;
                press_started_in_standby = true; // 复用这个标志，避免立即开始输入
                
                g_press_preview_state.active = false;
                g_press_preview_state.duration_ms = 0;
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
#if MORSE_CODE_INPUT_SHOW_MODE == 2
                g_morse_state.full_column_breathing_active = false;
                g_morse_state.full_column_breathing_last_update = 0;
                g_morse_state.full_column_breathing_phase = 0;
                g_morse_state.max_filled_rows = 0;
#endif
                g_morse_state.decode_waiting = false;
                g_morse_state.decode_wait_start = 0;
                
                ESP_LOGI(TAG, "Display stopped, ready for morse input");
            } else {
                // 正常的按键处理流程
                press_started_in_standby = was_standby;

                g_press_preview_state.active = true;
                g_press_preview_state.symbol = '.';
                g_press_preview_state.brightness = 1;
                g_press_preview_state.duration_ms = 0;
#if MORSE_CODE_INPUT_SHOW_MODE == 2
                g_morse_state.full_column_breathing_active = false;
                g_morse_state.full_column_breathing_last_update = 0;
                g_morse_state.full_column_breathing_phase = 0;
                g_morse_state.max_filled_rows = 0;
#endif
                if (press_started_in_standby) {
                    g_press_preview_state.active = false;
                    g_press_preview_state.duration_ms = 0;
                    g_morse_state.waiting_last_update = 0;
                    g_morse_state.waiting_breathing_phase = 0;
                    morsematrix->clear_all();
                    screen_is_empty = true;
                } else {
#if MORSE_CODE_INPUT_SHOW_MODE == 2
                    // MODE2: 开始新输入前清理所有显示状态
                    // 停止等待动画
                    g_morse_state.waiting_animation_active = false;
                    g_morse_state.waiting_last_update = 0;
                    g_morse_state.waiting_breathing_phase = 0;
                    
                    // 停止IR显示
                    stop_ir_display_thread();
                    
                    // 清除已解码文本显示
                    decoded_text[0] = '\0';
                    
                    // 重置呼吸效果
                    morsematrix->reset_breathing_effect();
                    
                    // 清屏并开始显示摩尔斯输入
                    morsematrix->clear_all();
#endif
                    morsematrix->display_morse_input(morse_buffer);
                    screen_is_empty = false;
                }

                ESP_LOGI(TAG, "Key pressed");
            }
        }
        else if (event == m5_morsematrix_button::EVENT_PRESS_TICK) {
            last_user_interaction = now;

            if (!morse_button.get_state()) {
                continue;
            }

            if (press_started_in_standby) {
                g_press_preview_state.active = false;
                g_press_preview_state.duration_ms = 0;
                screen_is_empty = true;
                continue;
            }

            uint32_t press_duration_ms = morse_button.get_press_duration_ms();

            if (press_duration_ms >= MORSE_RESET_PRESS_MIN) {
#if MORSE_CODE_INPUT_SHOW_MODE == 2
                g_press_preview_state.active = true;
                g_press_preview_state.symbol = '-';
                g_press_preview_state.brightness = kMorseDashBrightnessLevel;
                g_press_preview_state.duration_ms = press_duration_ms;
                morsematrix->display_morse_input(morse_buffer);
                screen_is_empty = false;
#else
                if (g_press_preview_state.active) {
                    g_press_preview_state.active = false;
                    g_press_preview_state.duration_ms = 0;
                }
                update_hold_breathing_column(morsematrix, hold_breathing_last_update,
                                             hold_breathing_phase, kMorseDashBrightnessLevel);
                screen_is_empty = false;
#endif
            } else {
                hold_breathing_last_update = 0;
                hold_breathing_phase = 0;

                g_press_preview_state.active = true;
                g_press_preview_state.duration_ms = press_duration_ms;

                if (press_duration_ms >= MORSE_LONG_PRESS_MIN) {
                    g_press_preview_state.symbol = '-';
                    g_press_preview_state.brightness = kMorseDashBrightnessLevel;
                } else {
                    g_press_preview_state.symbol = '.';
                    float ratio = static_cast<float>(press_duration_ms) /
                        static_cast<float>(MORSE_SHORT_PRESS_MAX);
                    if (ratio > 1.0f) {
                        ratio = 1.0f;
                    } else if (ratio < 0.0f) {
                        ratio = 0.0f;
                    }
                    float brightness_f = 1.0f +
                        ratio * (static_cast<float>(kMorseDotBrightnessLevel) - 1.0f);
                    uint8_t brightness = static_cast<uint8_t>(brightness_f + 0.5f);
                    if (brightness == 0) {
                        brightness = 1;
                    }
                    if (brightness >= m5_morsematrix_component::kBrightnessLevelCount) {
                        brightness = static_cast<uint8_t>(m5_morsematrix_component::kBrightnessLevelCount - 1);
                    }
                    g_press_preview_state.brightness = brightness;
                }

                morsematrix->display_morse_input(morse_buffer);
                screen_is_empty = false;
            }

            continue;
        }
        else if (event == m5_morsematrix_button::EVENT_SHORT_PRESS) {
            ESP_LOGI(TAG, "EVENT_SHORT_PRESS");
            last_user_interaction = now;
            hold_breathing_last_update = 0;
            hold_breathing_phase = 0;

            if (press_started_in_standby) {
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
                screen_is_empty = true;
                press_started_in_standby = false;
                g_press_preview_state.active = false;
                g_press_preview_state.duration_ms = 0;
                continue;
            }

            press_started_in_standby = false;

            size_t len = strlen(morse_buffer);
            if (len < sizeof(morse_buffer) - 1) {
                morse_buffer[len] = '.';
                morse_buffer[len + 1] = '\0';
                uint32_t press_duration_ms = morse_button.get_last_press_duration_ms();
                if (press_duration_ms > 0xFFFFu) {
                    press_duration_ms = 0xFFFFu;
                }
                if (len < kMorseBufferCapacity) {
                    g_press_duration_history[len] = static_cast<uint16_t>(press_duration_ms);
                    if (len + 1 < kMorseBufferCapacity) {
                        g_press_duration_history[len + 1] = 0;
                    }
                }
                last_input_time = now;
                need_decode = true;
                screen_is_empty = false;
                g_morse_state.decode_waiting = false;
                g_morse_state.decode_wait_start = 0;
                ESP_LOGI(TAG, "Added DOT(.), buffer: %s", morse_buffer);
#if MORSE_CODE_INPUT_SHOW_MODE == 2
                // 第一次显示：保持preview状态，显示完整长度
                morsematrix->display_morse_input(morse_buffer);
                // 然后关闭preview并再次刷新以应用缩回效果
                g_press_preview_state.active = false;
                g_press_preview_state.duration_ms = 0;
                vTaskDelay(pdMS_TO_TICKS(1));
                morsematrix->display_morse_input(morse_buffer);
#else
                g_press_preview_state.active = false;
                g_press_preview_state.duration_ms = 0;
                morsematrix->display_morse_input(morse_buffer);
#endif
            }
        }
        else if (event == m5_morsematrix_button::EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "EVENT_LONG_PRESS");
            last_user_interaction = now;
            hold_breathing_last_update = 0;
            hold_breathing_phase = 0;

            if (press_started_in_standby) {
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
                screen_is_empty = true;
                g_press_preview_state.active = false;
                g_press_preview_state.duration_ms = 0;
                continue;
            }

            press_started_in_standby = false;

            if (g_morse_state.standby_active) {
                exit_standby_animation(morsematrix);
            }
            size_t len = strlen(morse_buffer);
            if (len < sizeof(morse_buffer) - 1) {
                morse_buffer[len] = '-';
                morse_buffer[len + 1] = '\0';
                uint32_t press_duration_ms = morse_button.get_last_press_duration_ms();
                if (press_duration_ms > 0xFFFFu) {
                    press_duration_ms = 0xFFFFu;
                }
                if (len < kMorseBufferCapacity) {
                    g_press_duration_history[len] = static_cast<uint16_t>(press_duration_ms);
                    if (len + 1 < kMorseBufferCapacity) {
                        g_press_duration_history[len + 1] = 0;
                    }
                }
                last_input_time = now;
                need_decode = true;
                screen_is_empty = false;
                g_morse_state.decode_waiting = false;
                g_morse_state.decode_wait_start = 0;
                ESP_LOGI(TAG, "Added DASH(-), buffer: %s", morse_buffer);
#if MORSE_CODE_INPUT_SHOW_MODE == 2
                // 第一次显示：保持preview状态，显示完整长度
                morsematrix->display_morse_input(morse_buffer);
                // 然后关闭preview并再次刷新以应用缩回效果
                g_press_preview_state.active = false;
                g_press_preview_state.duration_ms = 0;
                vTaskDelay(pdMS_TO_TICKS(1));
                morsematrix->display_morse_input(morse_buffer);
#else
                g_press_preview_state.active = false;
                g_press_preview_state.duration_ms = 0;
                morsematrix->display_morse_input(morse_buffer);
#endif
            }
        }
        else if (event == m5_morsematrix_button::EVENT_VERY_LONG_PRESS) {
            ESP_LOGI(TAG, "EVENT_VERY_LONG_PRESS");
            last_user_interaction = now;
            g_press_preview_state.active = false;
            g_press_preview_state.duration_ms = 0;
            hold_breathing_last_update = 0;
            hold_breathing_phase = 0;

            if (press_started_in_standby) {
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
                screen_is_empty = true;
                continue;
            }

            press_started_in_standby = false;

#if MORSE_CODE_INPUT_SHOW_MODE == 2
            // MODE2: 直接清屏并进入待机
            ESP_LOGI(TAG, "MODE2: RESET - Full column breathing clear");
            morse_buffer[0] = '\0';
            decoded_text[0] = '\0';
            memset(g_press_duration_history, 0, sizeof(g_press_duration_history));
            need_decode = false;
            last_input_time = 0;
            g_morse_state.full_column_breathing_active = false;
            g_morse_state.decode_waiting = false;
            g_morse_state.decode_wait_start = 0;

            reset_ir_runtime_state(morsematrix, true);

            auto clean_text_callback = morsematrix->morse_clean_text_callback();
            if (clean_text_callback) {
                uint32_t press_duration_s = morse_button.get_last_press_duration_ms() / 1000;
                clean_text_callback(press_duration_s);
            }

            morsematrix->display_morse_input(morse_buffer);
            morsematrix->display_decoded_text(decoded_text);
            morsematrix->reset_breathing_effect();

            g_morse_state.standby_mode = choose_random_standby_mode();
            enter_standby_animation(morsematrix, g_morse_state.standby_mode);
            screen_is_empty = false;
            last_user_interaction = now;
#else
            if (g_morse_state.standby_active) {
                exit_standby_animation(morsematrix);
            }
            morse_buffer[0] = '\0';
            decoded_text[0] = '\0';
            memset(g_press_duration_history, 0, sizeof(g_press_duration_history));
            need_decode = false;
            screen_is_empty = true;
            g_morse_state.decode_waiting = false;
            g_morse_state.decode_wait_start = 0;
            ESP_LOGI(TAG, "RESET - All cleared");

            reset_ir_runtime_state(morsematrix, true);

            auto clean_text_callback = morsematrix->morse_clean_text_callback();
            if (clean_text_callback) {
                uint32_t press_duration_s = morse_button.get_last_press_duration_ms() / 1000;
                clean_text_callback(press_duration_s);
            }

            morsematrix->display_morse_input(morse_buffer);
            morsematrix->display_decoded_text(decoded_text);
            morsematrix->reset_breathing_effect();

            enter_standby_animation(morsematrix, g_morse_state.standby_mode);
            screen_is_empty = false;
            last_user_interaction = now;
#endif
        }
        else if (event == m5_morsematrix_button::EVENT_RELEASED) {
            ESP_LOGI(TAG, "EVENT_RELEASED");
            g_press_preview_state.active = false;
            g_press_preview_state.duration_ms = 0;
            hold_breathing_last_update = 0;
            hold_breathing_phase = 0;

            if (press_started_in_standby) {
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
                morsematrix->clear_all();
                screen_is_empty = true;
                press_started_in_standby = false;
                continue;
            }

            press_started_in_standby = false;
#if MORSE_CODE_INPUT_SHOW_MODE == 2
            ESP_LOGI(TAG, "RELEASED: full_column_breathing_active=%d", g_morse_state.full_column_breathing_active);
            if (g_morse_state.full_column_breathing_active) {
                ESP_LOGI(TAG, "RESET - Full column breathing clear");
                morse_buffer[0] = '\0';
                decoded_text[0] = '\0';
                memset(g_press_duration_history, 0, sizeof(g_press_duration_history));
                need_decode = false;
                last_input_time = 0;
                g_morse_state.full_column_breathing_active = false;

                reset_ir_runtime_state(morsematrix, true);

                auto clean_text_callback = morsematrix->morse_clean_text_callback();
                if (clean_text_callback) {
                    uint32_t press_duration_s = morse_button.get_last_press_duration_ms() / 1000;
                    clean_text_callback(press_duration_s);
                }

                morsematrix->display_morse_input(morse_buffer);
                morsematrix->display_decoded_text(decoded_text);
                morsematrix->reset_breathing_effect();

                g_morse_state.standby_mode = choose_random_standby_mode();
                enter_standby_animation(morsematrix, g_morse_state.standby_mode);
                screen_is_empty = false;
                last_user_interaction = now;
                continue;
            }
#endif
        }
        
        // 自动解码
        if (!morse_button.get_state() && need_decode && strlen(morse_buffer) > 0) {
            uint32_t idle_time_ms = pdTICKS_TO_MS(now - last_input_time);

            if (!g_morse_state.decode_waiting) {
                if (idle_time_ms >= MORSE_CHAR_TIMEOUT) {
                    g_morse_state.decode_waiting = true;
                    g_morse_state.decode_wait_start = now;
                    ESP_LOGI(TAG, "Decode pending - stabilizing input for %ums", static_cast<unsigned>(kMorseDecodeStabilizeDelayMs));
                }
            } else {
                uint32_t wait_ms = pdTICKS_TO_MS(now - g_morse_state.decode_wait_start);
                if (wait_ms >= kMorseDecodeStabilizeDelayMs) {
                    const char* decoded = morsematrix->decode_morse(morse_buffer);
                    size_t len = strlen(decoded_text);
                    if (len < sizeof(decoded_text) - 1) {
                        decoded_text[len] = decoded[0];
                        decoded_text[len + 1] = '\0';
                    }

                    ESP_LOGI(TAG, "Decoded: %s -> %s", morse_buffer, decoded);

                    morse_buffer[0] = '\0';
                    memset(g_press_duration_history, 0, sizeof(g_press_duration_history));
                    need_decode = false;
                    screen_is_empty = false;
                    g_morse_state.decode_waiting = false;
                    g_morse_state.decode_wait_start = 0;
                    morsematrix->display_morse_input(morse_buffer);
                    morsematrix->display_decoded_text(decoded_text);
                }
            }
        } else {
            if (g_morse_state.decode_waiting) {
                g_morse_state.decode_waiting = false;
                g_morse_state.decode_wait_start = 0;
            }
        }
        
        // 等待输入动画
        if (g_morse_state.direct_text_active) {
            screen_is_empty = false;
        }
        if (screen_is_empty && !g_morse_state.standby_active) {
            g_morse_state.waiting_animation_active = true;
            g_morse_state.external_input_ready = true;
            TickType_t waiting_now = xTaskGetTickCount();
            if (g_morse_state.waiting_last_update == 0) {
                g_morse_state.waiting_last_update = waiting_now;
            }

            uint32_t waiting_delta_ms = pdTICKS_TO_MS(waiting_now - g_morse_state.waiting_last_update);
            g_morse_state.waiting_last_update = waiting_now;

            // 周期减半(1000ms),速度加快一倍
            constexpr uint32_t waiting_period_ms = 1000;
            constexpr float waiting_degrees_per_ms = 360.0f / static_cast<float>(waiting_period_ms);

            float waiting_phase_increment = static_cast<float>(waiting_delta_ms) * waiting_degrees_per_ms;
            g_morse_state.waiting_breathing_phase =
                (g_morse_state.waiting_breathing_phase + static_cast<uint32_t>(waiting_phase_increment)) % 360;

            // 使用正态分布(高斯分布)曲线: exp(-x^2)
            // 将相位映射到 [-2.5, 2.5] 区间,在0点最亮,两端渐暗
            float normalized_phase = static_cast<float>(g_morse_state.waiting_breathing_phase) / 180.0f - 1.0f; // [-1, 1]
            float gaussian_x = normalized_phase * 2.5f; // 扩展到 [-2.5, 2.5] 以获得更明显的渐变
            float gaussian_value = expf(-gaussian_x * gaussian_x); // 正态分布曲线 [0, 1]

            // 映射到256阶梯度，最低亮度使用 kBrightnessLevels[0]（完全熄灯）
            // gradient_step 范围从 0 到 255（对应 level 0 到 level 7）
            constexpr uint8_t kMinGradient = 0;   // 对应 kBrightnessLevels[0]
            constexpr uint8_t kMaxGradient = 255; // 对应 kBrightnessLevels[7]
            uint8_t gradient_step = kMinGradient + static_cast<uint8_t>(gaussian_value * static_cast<float>(kMaxGradient - kMinGradient));

            // 使用get_interpolated_brightness获取真正的256阶插值PWM参数
            uint32_t on_time_us = 0;
            uint32_t off_time_us = 0;
            m5_morsematrix_component::get_interpolated_brightness(gradient_step, &on_time_us, &off_time_us);

            // 直接使用任意PWM时序，实现真正的256阶平滑亮度过渡
            morsematrix->clear_all();
            morsematrix->set_pixel_brightness_custom(3, 0, on_time_us, off_time_us);
            morsematrix->set_pixel_brightness_custom(4, 0, on_time_us, off_time_us);
        } else {
            if (g_morse_state.waiting_animation_active) {
                g_morse_state.waiting_animation_active = false;
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
            }
            g_morse_state.external_input_ready = false;
        }
        
        // 处理USB串口输入
        process_usb_input();
        
        // IR接收显示处理（同时也处理USB串口输入）
        if (g_morse_state.has_ir_text_to_display) {
            g_morse_state.waiting_animation_active = false;
            g_morse_state.waiting_last_update = 0;
            g_morse_state.waiting_breathing_phase = 0;
            g_morse_state.external_input_ready = false;

            morsematrix->reset_breathing_effect();
            morsematrix->clear_all();

            ESP_LOGI(TAG, "Displaying received text...");
            display_ir_received_text();
            g_morse_state.has_ir_text_to_display = false;

            screen_is_empty = false;
        }

        if (!g_morse_state.standby_active && screen_is_empty) {
            TickType_t reference_tick = last_user_interaction;
            if (g_morse_state.last_ir_activity_tick > reference_tick) {
                reference_tick = g_morse_state.last_ir_activity_tick;
            }

            uint32_t idle_ms = pdTICKS_TO_MS(now - reference_tick);
            constexpr uint32_t kStandbyIdleTimeoutMs = 30000;
            if (idle_ms >= kStandbyIdleTimeoutMs) {
                enter_standby_animation(morsematrix, g_morse_state.standby_mode);
                g_morse_state.waiting_last_update = 0;
                g_morse_state.waiting_breathing_phase = 0;
                screen_is_empty = false;
                last_user_interaction = now;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
