/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "m5_morsematrix_component.h"
#include "freertos/idf_additions.h"
#include "esp_intr_alloc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
// FreeRTOS primitives used for deferring IR-triggered display work
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include <cstring>
#include <cstdlib>

static const char *TAG = "m5_morsematrix_component";

m5_morsematrix_component* m5_morsematrix_component::instance_ = nullptr;

m5_morsematrix_component* m5_morsematrix_component::instance()
{
    return instance_;
}

bool m5_morsematrix_component::has_instance()
{
    return instance_ != nullptr;
}

m5_morsematrix_button& m5_morsematrix_component::button_singleton()
{
    auto* inst = instance();
    if (!inst) {
        ESP_LOGE(TAG, "button_singleton accessed before component initialization");
        abort();
    }
    return inst->button_;
}

const m5_morsematrix_button& m5_morsematrix_component::button_singleton_const()
{
    auto* inst = instance();
    if (!inst) {
        ESP_LOGE(TAG, "button_singleton_const accessed before component initialization");
        abort();
    }
    return inst->button_;
}

m5_morsematrix_ir& m5_morsematrix_component::ir_singleton()
{
    auto* inst = instance();
    if (!inst) {
        ESP_LOGE(TAG, "ir_singleton accessed before component initialization");
        abort();
    }
    return inst->ir_module_;
}

const m5_morsematrix_ir& m5_morsematrix_component::ir_singleton_const()
{
    auto* inst = instance();
    if (!inst) {
        ESP_LOGE(TAG, "ir_singleton_const accessed before component initialization");
        abort();
    }
    return inst->ir_module_;
}

m5_morsematrix_wifi& m5_morsematrix_component::wifi_singleton()
{
    auto* inst = instance();
    if (!inst) {
        ESP_LOGE(TAG, "wifi_singleton accessed before component initialization");
        abort();
    }
    return inst->wifi_module_;
}

const m5_morsematrix_wifi& m5_morsematrix_component::wifi_singleton_const()
{
    auto* inst = instance();
    if (!inst) {
        ESP_LOGE(TAG, "wifi_singleton_const accessed before component initialization");
        abort();
    }
    return inst->wifi_module_;
}

namespace {

inline uint32_t to_pin_mask(gpio_num_t pin) {
    int pin_index = static_cast<int>(pin);
    if (pin_index < 0 || pin_index > 31) {
        return 0U;
    }
    return 1u << pin_index;
}

// Minimal helper to issue a single write to a GPIO register.
inline void write_reg(uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(reg) = value;
}

// Leverage write-one-to-set/clear registers to toggle multiple pins at once.
inline void fast_drive_pins(uint32_t high_mask, uint32_t low_mask) {
    if (low_mask) {
        write_reg(GPIO_OUT_W1TC_REG, low_mask);
    }
    if (high_mask) {
        write_reg(GPIO_OUT_W1TS_REG, high_mask);
    }
    uint32_t enable_mask = high_mask | low_mask;
    if (enable_mask) {
        write_reg(GPIO_ENABLE_W1TS_REG, enable_mask);
    }
}

inline void fast_disable_pins(uint32_t mask) {
    if (mask) {
        write_reg(GPIO_OUT_W1TC_REG, mask);
        write_reg(GPIO_ENABLE_W1TC_REG, mask);
    }
}

} // namespace

// m5_morsematrix_component 构造函数
m5_morsematrix_component::m5_morsematrix_component()
{
    instance_ = this;
    ir_module_.set_owner(this);
    wifi_module_.set_owner(this);
}

// m5_morsematrix_component 析构函数
m5_morsematrix_component::~m5_morsematrix_component()
{
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

// 按键处理函数（在任务上下文被调用）
void m5_morsematrix_component::on_button_pressed(gpio_num_t pin)
{
    ESP_LOGI(TAG, "Button short pressed on pin %d", pin);
    // 示例：点亮第0-63个LED，保持500ms
    this->light_leds_range(0, 63, 500);
}

// 按键长按处理函数（在任务上下文被调用）
void m5_morsematrix_component::on_button_long_pressed()
{
    ESP_LOGI(TAG, "Button long pressed (>2s), entering sleep mode");
    
    // 播放关机动画
    play_shutdown_animation();
    
    // 配置GPIO唤醒
    gpio_wakeup_enable(MORSEMATRIX_PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(MORSEMATRIX_PIN_IR_RX, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    
    // 进入轻度休眠
    ESP_LOGI(TAG, "Entering light sleep...");
    esp_light_sleep_start();
    
    // 被唤醒后播放开机动画
    ESP_LOGI(TAG, "Woken up from light sleep!");
    play_startup_animation();
}

// 关机动画：点亮64个LED,然后从尾部快速往前熄灭
void m5_morsematrix_component::play_shutdown_animation()
{
    ESP_LOGI(TAG, "Playing shutdown animation...");
    
    const uint32_t kStepUs = 20000; // 每个LED熄灭间隔20ms
    TickType_t step_ticks = pdMS_TO_TICKS(kStepUs / 1000);
    if (step_ticks == 0) {
        step_ticks = 1;
    }

    for (int8_t index = 63; index >= 0; index--) {
        uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
        for (uint8_t i = 0; i <= index; i++) {
            uint8_t y = i / kMatrixWidth;
            uint8_t x = i % kMatrixWidth;
            frame[y][x] = default_brightness_level_;
        }

        apply_frame(frame);
        vTaskDelay(step_ticks);
    }

    clear_all();
    ESP_LOGI(TAG, "Shutdown animation complete");
}

// 开机动画：从头快速点亮64个LED,全部点亮后维持2s,然后关闭
void m5_morsematrix_component::play_startup_animation()
{
    ESP_LOGI(TAG, "Playing startup animation...");
    
    const uint32_t kStepUs = 20000; // 每个LED点亮间隔20ms
    TickType_t step_ticks = pdMS_TO_TICKS(kStepUs / 1000);
    if (step_ticks == 0) {
        step_ticks = 1;
    }

    for (uint8_t index = 0; index < 64; index++) {
        uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
        for (uint8_t i = 0; i <= index; i++) {
            uint8_t y = i / kMatrixWidth;
            uint8_t x = i % kMatrixWidth;
            frame[y][x] = default_brightness_level_;
        }

        apply_frame(frame);
        vTaskDelay(step_ticks);
    }

    ESP_LOGI(TAG, "All LEDs on, holding for 2s");
    TickType_t hold_ticks = pdMS_TO_TICKS(2000);
    if (hold_ticks == 0) {
        hold_ticks = 1;
    }
    vTaskDelay(hold_ticks);

    clear_all();
    ESP_LOGI(TAG, "Startup animation complete");
}

void m5_morsematrix_component::setup_ir_for_led_show_task()
{
    instance_ = this;
    ir_module_.setup_led_show_task();
}

void m5_morsematrix_component::setup_ir_for_morse_task()
{
    instance_ = this;
    ir_module_.setup_morse_task();
}

bool m5_morsematrix_component::read_ir_morse_packet(uint8_t* sequence, uint8_t* data)
{
    return ir_module_.read_morse_packet(sequence, data);
}

size_t m5_morsematrix_component::get_ir_morse_packet_count()
{
    return ir_module_.get_morse_packet_count();
}

// ============================================================================
// WiFi Connectivity (Delegated to wifi_module_)
// ============================================================================

esp_err_t m5_morsematrix_component::wifi_init_and_connect(const char* ssid, const char* password)
{
    wifi_module_.set_owner(this);
    return wifi_module_.init_and_connect(ssid, password);
}

bool m5_morsematrix_component::is_wifi_connected()
{
    return wifi_module_.is_connected();
}

esp_err_t m5_morsematrix_component::start_refresh_task()
{
    if (refresh_task_handle_) {
        ESP_LOGI(TAG, "Refresh task already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Creating LED refresh task with priority %u...", static_cast<unsigned>(kRefreshTaskPriority));
    refresh_task_running_ = true;
    BaseType_t res = xTaskCreate(
        &m5_morsematrix_component::refresh_task,
        "m5_led_refresh",
        4096,
        this,
        kRefreshTaskPriority,
        &refresh_task_handle_);

    if (res != pdPASS) {
        refresh_task_running_ = false;
        refresh_task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to create LED refresh task, result: %d", res);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LED refresh task created successfully");
    return ESP_OK;
}

void m5_morsematrix_component::refresh_task(void* arg)
{
    auto* instance = static_cast<m5_morsematrix_component*>(arg);
    if (!instance) {
        ESP_LOGE(TAG, "Refresh task received null instance pointer!");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "LED refresh task started, entering main loop...");

    uint32_t slot_on_lookup[kBrightnessLevelCount] = {};
    uint32_t slot_off_lookup[kBrightnessLevelCount] = {};
    // 预先缓存每个亮度等级的导通/关断时长，避免在内层循环反复计算

    for (uint8_t level = 0; level < kBrightnessLevelCount; ++level) {
        uint32_t on_time = compute_slot_on_time(level);
        slot_on_lookup[level] = on_time;
        slot_off_lookup[level] = kSlotBaseUs - on_time;
    }

    instance->set_all_pins_highz();

    gpio_num_t active_anode = GPIO_NUM_NC;
    gpio_num_t active_cathode = GPIO_NUM_NC;

    const uint32_t tick_duration_us = 1000000U / configTICK_RATE_HZ;
    TickType_t frame_period_ticks = static_cast<TickType_t>(kFramePeriodUs / tick_duration_us);
    if (frame_period_ticks == 0) {
        frame_period_ticks = 1;
    }
    const uint32_t frame_period_tick_us = static_cast<uint32_t>(frame_period_ticks) * tick_duration_us;
    const uint32_t frame_fraction_us = (frame_period_tick_us >= kFramePeriodUs)
                                           ? 0U
                                           : (kFramePeriodUs - frame_period_tick_us);
    constexpr uint32_t kInterSlotBlankingUs = 2U;

    auto deactivate_current = [&]() {
        // 关闭当前激活的阳极/阴极，防止跨像素串扰
        uint32_t disable_mask = to_pin_mask(active_anode) | to_pin_mask(active_cathode);
        if (!disable_mask) {
            return;
        }

        fast_disable_pins(disable_mask);

        if (active_anode != GPIO_NUM_NC) {
            instance->mark_pin_output(active_anode, false);
            active_anode = GPIO_NUM_NC;
        }
        if (active_cathode != GPIO_NUM_NC) {
            instance->mark_pin_output(active_cathode, false);
            active_cathode = GPIO_NUM_NC;
        }
    };

    uint8_t local_frame[kMatrixHeight][kMatrixWidth] = {};
    TickType_t last_wake_tick = xTaskGetTickCount();

    while (instance->refresh_task_running_) {
        uint64_t frame_start_us = esp_timer_get_time();

        if (instance->frame_mutex_) {
            // 将当前帧一次性拷贝到局部缓存，降低刷新期间的互斥锁占用
            if (xSemaphoreTake(instance->frame_mutex_, portMAX_DELAY) == pdTRUE) {
                memcpy(local_frame, instance->frame_buffer_, sizeof(local_frame));
                xSemaphoreGive(instance->frame_mutex_);
            }
        } else {
            memcpy(local_frame, instance->frame_buffer_, sizeof(local_frame));
        }

        for (uint8_t y = 0; y < kMatrixHeight; ++y) {
            for (uint8_t x = 0; x < kMatrixWidth; ++x) {
                uint32_t on_time_us = 0;
                uint32_t off_time_us = 0;
                
                // 优先使用自定义PWM时序
                if (instance->use_custom_pwm_[y][x]) {
                    on_time_us = instance->custom_pwm_buffer_[y][x].on_time_us;
                    off_time_us = instance->custom_pwm_buffer_[y][x].off_time_us;
                } else {
                    // 使用8级索引查表
                    uint8_t level = local_frame[y][x];
                    if (level >= kBrightnessLevelCount) {
                        level = kBrightnessLevelCount - 1;
                    }
                    on_time_us = slot_on_lookup[level];
                    off_time_us = slot_off_lookup[level];
                }

                if (off_time_us > kInterSlotBlankingUs) {
                    off_time_us -= kInterSlotBlankingUs;
                } else {
                    off_time_us = 0;
                }

                deactivate_current();
                if (kInterSlotBlankingUs > 0) {
                    esp_rom_delay_us(kInterSlotBlankingUs);
                }

                if (on_time_us > 0) {
                    const led_map_t& led = led_map[y][x];
                    uint32_t anode_mask = to_pin_mask(led.anode_pin);
                    uint32_t cathode_mask = to_pin_mask(led.cathode_pin);

                    fast_drive_pins(anode_mask, cathode_mask);

                    instance->mark_pin_output(led.anode_pin, true);
                    instance->mark_pin_output(led.cathode_pin, true);

                    active_anode = led.anode_pin;
                    active_cathode = led.cathode_pin;

                    esp_rom_delay_us(on_time_us);

                    deactivate_current();
                }

                if (off_time_us > 0) {
                    esp_rom_delay_us(off_time_us);
                }
            }
        }

        uint32_t fractional_budget_us = frame_fraction_us;
        uint32_t extra_idle_us = instance->frame_idle_extra_us_;
        if (extra_idle_us > 0) {
            fractional_budget_us += extra_idle_us;
        }

        if (fractional_budget_us > 0) {
            uint64_t elapsed_us = esp_timer_get_time() - frame_start_us;
            if (elapsed_us < fractional_budget_us) {
                esp_rom_delay_us(static_cast<uint32_t>(fractional_budget_us - elapsed_us));
            }
        }

        TickType_t delay_ticks = frame_period_ticks + instance->frame_idle_min_ticks_;
        if (delay_ticks == 0) {
            delay_ticks = 1;
        }
        vTaskDelayUntil(&last_wake_tick, delay_ticks);
    }

    deactivate_current();
    instance->set_all_pins_highz();
    instance->refresh_task_handle_ = nullptr;
    instance->refresh_task_running_ = false;
    vTaskDelete(nullptr);
}

void m5_morsematrix_component::set_frame_idle_budget(TickType_t min_idle_ticks, uint32_t extra_idle_us)
{
    if (frame_mutex_) {
        if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
            frame_idle_min_ticks_ = min_idle_ticks;
            frame_idle_extra_us_ = extra_idle_us;
            xSemaphoreGive(frame_mutex_);
        }
    } else {
        frame_idle_min_ticks_ = min_idle_ticks;
        frame_idle_extra_us_ = extra_idle_us;
    }
}

esp_err_t m5_morsematrix_component::init()
{
    if (!frame_mutex_) {
        frame_mutex_ = xSemaphoreCreateMutex();
        if (!frame_mutex_) {
            ESP_LOGE(TAG, "Failed to create frame mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // 初始化 GPIO引脚
    esp_err_t err = gpio_init();
    if (err != ESP_OK) {
        return err;
    }

    clear_all();
    ESP_LOGI(TAG, "GPIO initialized, clearing display, starting refresh task...");

    err = start_refresh_task();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start refresh task, error: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Refresh task started successfully");
    ESP_LOGI(TAG, "MorseMatrix initialized successfully, ready for use");
    
    return ESP_OK;
}

esp_err_t m5_morsematrix_component::gpio_init()
{
    ESP_LOGI(TAG, "m5_morsematrix_component GPIO initialization start.");
    
    gpio_config_t io_conf = {};
    
    // 配置所有使用的GPIO引脚
    uint64_t pin_mask = 0;
    
    // 添加所有行列引脚到mask (GPIO 0-7, 10)
    pin_mask |= (1ULL << GPIO_NUM_0);
    pin_mask |= (1ULL << GPIO_NUM_1);
    pin_mask |= (1ULL << GPIO_NUM_2);
    pin_mask |= (1ULL << GPIO_NUM_3);
    pin_mask |= (1ULL << GPIO_NUM_4);
    pin_mask |= (1ULL << GPIO_NUM_5);
    pin_mask |= (1ULL << GPIO_NUM_6);
    pin_mask |= (1ULL << GPIO_NUM_7);
    pin_mask |= (1ULL << GPIO_NUM_10);
    
    io_conf.pin_bit_mask = pin_mask;
    io_conf.mode = GPIO_MODE_INPUT;  // 初始设为高阻态
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    
    gpio_config(&io_conf);
    
    // 配置按键引脚为输入,带上拉
    gpio_config_t btn_conf = {};
    btn_conf.pin_bit_mask = (1ULL << MORSEMATRIX_PIN_BUTTON);
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&btn_conf);
    
    // 配置IR接收引脚为输入
    gpio_config_t ir_conf = {};
    ir_conf.pin_bit_mask = (1ULL << MORSEMATRIX_PIN_IR_RX);
    ir_conf.mode = GPIO_MODE_INPUT;
    ir_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ir_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ir_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&ir_conf);
    
    ESP_LOGI(TAG, "m5_morsematrix_component GPIO initialization complete.");
    
    return ESP_OK;
}

void m5_morsematrix_component::set_all_pins_highz()
{
    uint32_t mask = row_output_mask_ | col_output_mask_;
    if (mask) {
        fast_disable_pins(mask);
        row_output_mask_ = 0;
        col_output_mask_ = 0;
    }

    memset(row_output_state_, 0, sizeof(row_output_state_));
    memset(col_output_state_, 0, sizeof(col_output_state_));
}

void m5_morsematrix_component::mark_pin_output(gpio_num_t pin, bool is_output)
{
    for (int i = 0; i < 9; ++i) {
        if (row_pins[i] == pin) {
            row_output_state_[i] = is_output;
            uint32_t mask = to_pin_mask(pin);
            if (is_output) {
                row_output_mask_ |= mask;
            } else {
                row_output_mask_ &= ~mask;
            }
            return;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (col_pins[i] == pin) {
            col_output_state_[i] = is_output;
            uint32_t mask = to_pin_mask(pin);
            if (is_output) {
                col_output_mask_ |= mask;
            } else {
                col_output_mask_ &= ~mask;
            }
            return;
        }
    }
}


bool m5_morsematrix_component::is_button_pressed() {
    return gpio_get_level(MORSEMATRIX_PIN_BUTTON) == 0;  // 按键按下时为低电平
}
const char* m5_morsematrix_component::handle_default_decode_morse(m5_morsematrix_component* self, const char* morse_code)
{
    (void)self;
    (void)morse_code;
    return "?";
}

void m5_morsematrix_component::handle_default_morse_clean_text(uint32_t duration_s)
{
    ESP_LOGI(TAG, "Morse clean text callback (default), duration=%lus", static_cast<unsigned long>(duration_s));

    // Default behavior: clear the display column without additional effects
    auto* component = instance();
    if (!component) {
        return;
    }

    component->reset_char_animation_state();
    component->display_morse_input("");
    component->display_decoded_text("");
}

void m5_morsematrix_component::set_morse_clean_text_callback(void (*callback)(uint32_t duration_s))
{
    auto target_callback = callback ? callback : &m5_morsematrix_component::handle_default_morse_clean_text;

    if (frame_mutex_) {
        if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
            morse_clean_text_callback_ = target_callback;
            xSemaphoreGive(frame_mutex_);
        }
    } else {
        morse_clean_text_callback_ = target_callback;
    }
}

void m5_morsematrix_component::set_ir_decode_success_callback(void (*callback)(uint32_t reception_index, const char* decoded_text, uint16_t packet_count, float error_rate, bool parse_success))
{
    if (frame_mutex_) {
        if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
            ir_decode_success_callback_ = callback;
            xSemaphoreGive(frame_mutex_);
        }
    } else {
        ir_decode_success_callback_ = callback;
    }
}
