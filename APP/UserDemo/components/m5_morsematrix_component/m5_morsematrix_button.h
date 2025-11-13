/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __M5_MORSEMATRIX_BUTTON_H__
#define __M5_MORSEMATRIX_BUTTON_H__

#include <stdint.h>

#include "freertos/FreeRTOS.h"

/**
 * @brief 摩尔斯矩阵按键管理类
 * 提供防抖、状态检测和摩尔斯码输入支持
 */
class m5_morsematrix_button {
public:
	/**
	 * @brief 按键事件类型
	 */
	enum event_type_t {
		EVENT_NONE = 0,           // 无事件
		EVENT_PRESSED,            // 按下
		EVENT_PRESS_TICK,         // 按下维持期间的周期回调
		EVENT_RELEASED,           // 释放
		EVENT_SHORT_PRESS,        // 短按（点 .）
		EVENT_LONG_PRESS,         // 长按（划 -）
		EVENT_VERY_LONG_PRESS     // 超长按（重置）
	};

	/**
	 * @brief 构造函数
	 * @param debounce_ms 防抖时间（毫秒），默认15ms
	 * @param short_press_max_ms 短按最大时长（毫秒），默认300ms
	 * @param long_press_min_ms 长按最小时长（毫秒），默认300ms
	 * @param very_long_press_min_ms 超长按最小时长（毫秒），默认1500ms
	 */
	explicit m5_morsematrix_button(uint32_t debounce_ms = 15,
								   uint32_t short_press_max_ms = 300,
								   uint32_t long_press_min_ms = 300,
								   uint32_t very_long_press_min_ms = 1500);

	/**
	 * @brief 更新按键状态（必须周期性调用）
	 * @param raw_state 原始按键状态（true=按下，false=释放）
	 * @return true=状态已稳定并更新，false=仍在防抖中
	 */
	bool update(bool raw_state);

	/**
	 * @brief 获取当前稳定的按键状态
	 * @return true=按下，false=释放
	 */
	bool get_state() const { return current_state_; }

	/**
	 * @brief 获取并消费一个按键事件
	 * @return event_type_t 事件类型
	 */
	event_type_t get_event();

	/**
	 * @brief 检查状态是否发生了变化（在上次调用后）
	 */
	bool is_changed();

	/**
	 * @brief 检查是否刚按下（从释放到按下的边沿）
	 */
	bool is_pressed();

	/**
	 * @brief 检查是否刚释放（从按下到释放的边沿）
	 */
	bool is_released();

	/**
	 * @brief 获取当前按键按下的持续时间（毫秒）
	 * @return 持续时间，如果未按下返回0
	 */
	uint32_t get_press_duration_ms() const;

	/**
	 * @brief 获取上次按键释放时的持续时间（毫秒）
	 */
	uint32_t get_last_press_duration_ms() const { return last_press_duration_ms_; }

	/**
	 * @brief 检查是否为短按（点 .）
	 */
	bool is_short_press() const;

	/**
	 * @brief 检查是否为长按（划 -）
	 */
	bool is_long_press() const;

	/**
	 * @brief 检查是否为超长按（重置）
	 */
	bool is_very_long_press() const;

	/**
	 * @brief 重置所有状态
	 */
	void reset();

	/**
	 * @brief 设置时长阈值
	 */
	void set_thresholds(uint32_t short_max, uint32_t long_min, uint32_t very_long_min);

	/**
	 * @brief 设置按键防抖时间
	 * @param debounce_ms 防抖时间（毫秒）
	 */
	void set_debounce(uint32_t debounce_ms);

private:
	// 防抖参数
	uint32_t debounce_ms_;

	// 时长阈值（摩尔斯码相关）
	uint32_t short_press_max_ms_;
	uint32_t long_press_min_ms_;
	uint32_t very_long_press_min_ms_;

	// 状态变量
	bool last_raw_state_;
	bool current_state_;
	bool state_changed_;
	TickType_t last_change_time_;

	// 按压时间记录
	TickType_t press_start_time_;
	uint32_t last_press_duration_ms_;
	uint32_t press_tick_interval_ms_;
	TickType_t next_press_tick_time_;

	// 事件队列
	event_type_t pending_event_;
};

#endif // __M5_MORSEMATRIX_BUTTON_H__
