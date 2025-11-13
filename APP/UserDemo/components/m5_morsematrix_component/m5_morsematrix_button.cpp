/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "m5_morsematrix_button.h"
#include "freertos/task.h"

// ============================================================================
// m5_morsematrix_button Implementation
// ============================================================================

m5_morsematrix_button::m5_morsematrix_button(uint32_t debounce_ms,
                                             uint32_t short_press_max_ms,
                                             uint32_t long_press_min_ms,
                                             uint32_t very_long_press_min_ms)
    : debounce_ms_(debounce_ms)
    , short_press_max_ms_(short_press_max_ms)
    , long_press_min_ms_(long_press_min_ms)
    , very_long_press_min_ms_(very_long_press_min_ms)
    , last_raw_state_(false)
    , current_state_(false)
    , state_changed_(false)
    , last_change_time_(0)
    , press_start_time_(0)
    , last_press_duration_ms_(0)
    , press_tick_interval_ms_(100)
    , next_press_tick_time_(0)
    , pending_event_(EVENT_NONE)
{
}

bool m5_morsematrix_button::update(bool raw_state)
{
    TickType_t now = xTaskGetTickCount();

    // 检测原始状态变化
    if (raw_state != last_raw_state_) {
        last_raw_state_ = raw_state;
        last_change_time_ = now;
        return false;  // 状态变化，重新计时
    }

    // 检查是否超过防抖时间
    if (pdTICKS_TO_MS(now - last_change_time_) < debounce_ms_) {
        return false;  // 仍在防抖中
    }

    // 状态已稳定，检查是否与当前状态不同
    if (raw_state != current_state_) {
        bool old_state = current_state_;
        current_state_ = raw_state;
        state_changed_ = true;

        if (current_state_) {
            // 按下事件
            press_start_time_ = now;
            TickType_t interval_ticks = pdMS_TO_TICKS(press_tick_interval_ms_);
            if (interval_ticks == 0) {
                interval_ticks = 1;
            }
            next_press_tick_time_ = now + interval_ticks;
            pending_event_ = EVENT_PRESSED;
        } else {
            // 释放事件
            if (press_start_time_ != 0) {
                last_press_duration_ms_ = pdTICKS_TO_MS(now - press_start_time_);
                
                // 根据持续时间判断按键类型
                if (last_press_duration_ms_ >= very_long_press_min_ms_) {
                    pending_event_ = EVENT_VERY_LONG_PRESS;
                } else if (last_press_duration_ms_ >= long_press_min_ms_) {
                    pending_event_ = EVENT_LONG_PRESS;
                } else if (last_press_duration_ms_ < short_press_max_ms_) {
                    pending_event_ = EVENT_SHORT_PRESS;
                } else {
                    pending_event_ = EVENT_RELEASED;
                }
                
                press_start_time_ = 0;
            } else {
                pending_event_ = EVENT_RELEASED;
            }
            next_press_tick_time_ = 0;
        }

        return true;  // 状态已更新
    }

    if (current_state_) {
        // 长按期间定期触发 EVENT_PRESS_TICK，便于调用方实现进度提示
        TickType_t interval_ticks = pdMS_TO_TICKS(press_tick_interval_ms_);
        if (interval_ticks == 0) {
            interval_ticks = 1;
        }
        if (next_press_tick_time_ != 0 && now >= next_press_tick_time_) {
            if (pending_event_ == EVENT_NONE) {
                pending_event_ = EVENT_PRESS_TICK;
            }
            next_press_tick_time_ = now + interval_ticks;
            return true;
        }
    }

    return true;  // 状态稳定但未变化
}

m5_morsematrix_button::event_type_t m5_morsematrix_button::get_event()
{
    event_type_t event = pending_event_;
    pending_event_ = EVENT_NONE;
    return event;
}

bool m5_morsematrix_button::is_changed()
{
    bool changed = state_changed_;
    state_changed_ = false;  // 清除标志
    return changed;
}

bool m5_morsematrix_button::is_pressed()
{
    if (state_changed_ && current_state_) {
        state_changed_ = false;
        return true;
    }
    return false;
}

bool m5_morsematrix_button::is_released()
{
    if (state_changed_ && !current_state_) {
        state_changed_ = false;
        return true;
    }
    return false;
}

uint32_t m5_morsematrix_button::get_press_duration_ms() const
{
    if (!current_state_ || press_start_time_ == 0) {
        return 0;
    }
    
    TickType_t now = xTaskGetTickCount();
    return pdTICKS_TO_MS(now - press_start_time_);
}

bool m5_morsematrix_button::is_short_press() const
{
    return (last_press_duration_ms_ > 0 && 
            last_press_duration_ms_ < short_press_max_ms_);
}

bool m5_morsematrix_button::is_long_press() const
{
    return (last_press_duration_ms_ >= long_press_min_ms_ && 
            last_press_duration_ms_ < very_long_press_min_ms_);
}

bool m5_morsematrix_button::is_very_long_press() const
{
    return (last_press_duration_ms_ >= very_long_press_min_ms_);
}

void m5_morsematrix_button::reset()
{
    last_raw_state_ = false;
    current_state_ = false;
    state_changed_ = false;
    last_change_time_ = xTaskGetTickCount();
    press_start_time_ = 0;
    last_press_duration_ms_ = 0;
    press_tick_interval_ms_ = 100;
    next_press_tick_time_ = 0;
    pending_event_ = EVENT_NONE;
}

void m5_morsematrix_button::set_thresholds(uint32_t short_max, uint32_t long_min, uint32_t very_long_min)
{
    short_press_max_ms_ = short_max;
    long_press_min_ms_ = long_min;
    very_long_press_min_ms_ = very_long_min;
}

void m5_morsematrix_button::set_debounce(uint32_t debounce_ms)
{
    debounce_ms_ = debounce_ms;
}
