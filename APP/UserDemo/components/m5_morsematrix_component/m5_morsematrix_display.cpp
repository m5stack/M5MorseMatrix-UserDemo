/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "m5_morsematrix_component.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <cstdio>

static const char *TAG = "m5_morsematrix_component";

uint8_t m5_morsematrix_component::scale_brightness(uint8_t brightness, float ratio)
{
	if (ratio >= 1.0f) {
		return brightness;
	}
	if (ratio <= 0.0f) {
		return 0;
	}

	float scaled = static_cast<float>(brightness) * ratio;
	if (scaled < 1.0f && brightness > 0) {
		return 1;
	}

	uint8_t result = static_cast<uint8_t>(scaled + 0.5f);
	if (result > brightness) {
		result = brightness;
	}
	return result;
}

void m5_morsematrix_component::get_interpolated_brightness(uint8_t gradient_step,
														   uint32_t* out_on_time_us,
														   uint32_t* out_off_time_us)
{
	if (!out_on_time_us || !out_off_time_us) {
		return;
	}

	// gradient_step = 0: LED关闭
	if (gradient_step == 0) {
		*out_on_time_us = 0;
		*out_off_time_us = 1000;
		return;
	}

	// 在 kBrightnessLevels[1] (最暗可见) 到 kBrightnessLevels[7] (最亮) 之间插值
	// gradient_step 范围 [1, 255] 映射到 level [1, 7]
	
	// 定义插值区间
	constexpr uint8_t kMinLevel = 1;  // kBrightnessLevels[1] - 最暗可见亮度
	constexpr uint8_t kMaxLevel = 7;  // kBrightnessLevels[7] - 最亮亮度
	constexpr uint8_t kLevelRange = kMaxLevel - kMinLevel;  // 6个亮度等级的跨度
	
	// 计算在6个等级中的浮点位置 [0.0, 6.0]
	float position = (static_cast<float>(gradient_step) / 255.0f) * static_cast<float>(kLevelRange);
	
	// 确定所在的亮度区间
	uint8_t lower_level = kMinLevel + static_cast<uint8_t>(position);
	if (lower_level >= kMaxLevel) {
		// 达到或超过最大值，直接返回最亮等级
		*out_on_time_us = kBrightnessLevels[kMaxLevel].on_time_us;
		*out_off_time_us = kBrightnessLevels[kMaxLevel].off_time_us;
		return;
	}
	
	uint8_t upper_level = lower_level + 1;
	
	// 计算在当前区间内的插值比例 [0.0, 1.0]
	float local_ratio = position - static_cast<float>(lower_level - kMinLevel);
	
	// 获取区间两端的亮度参数
	const brightness_level_t& lower = kBrightnessLevels[lower_level];
	const brightness_level_t& upper = kBrightnessLevels[upper_level];
	
	// 线性插值 on_time 和 off_time
	float interpolated_on = static_cast<float>(lower.on_time_us) + 
							local_ratio * static_cast<float>(static_cast<int32_t>(upper.on_time_us) - 
															static_cast<int32_t>(lower.on_time_us));
	
	float interpolated_off = static_cast<float>(lower.off_time_us) + 
							 local_ratio * static_cast<float>(static_cast<int32_t>(upper.off_time_us) - 
															 static_cast<int32_t>(lower.off_time_us));
	
	// 转换为整数并确保有效范围
	*out_on_time_us = static_cast<uint32_t>(interpolated_on + 0.5f);
	*out_off_time_us = static_cast<uint32_t>(interpolated_off + 0.5f);
	
	// 确保最小值
	if (*out_on_time_us == 0 && gradient_step > 0) {
		*out_on_time_us = 1;
	}
	if (*out_off_time_us == 0) {
		*out_off_time_us = 1;
	}
}

uint8_t m5_morsematrix_component::gradient_to_brightness_level(uint8_t gradient_step)
{
	if (gradient_step == 0) {
		return 0;
	}
	
	// 将 [1, 255] 映射到 [1, 7]
	// 使用四舍五入获得最接近的等级
	constexpr uint8_t kMinLevel = 1;
	constexpr uint8_t kMaxLevel = 7;
	constexpr uint8_t kLevelRange = kMaxLevel - kMinLevel;
	
	float position = (static_cast<float>(gradient_step) / 255.0f) * static_cast<float>(kLevelRange);
	uint8_t level = kMinLevel + static_cast<uint8_t>(position + 0.5f);
	
	if (level > kMaxLevel) {
		level = kMaxLevel;
	}
	
	return level;
}

void m5_morsematrix_component::apply_frame(const uint8_t frame[kMatrixHeight][kMatrixWidth])
{
	if (!frame) {
		return;
	}

	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			apply_frame_locked(frame);
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		apply_frame_locked(frame);
	}
}

void m5_morsematrix_component::apply_frame(const uint8_t frame[kMatrixHeight][kMatrixWidth],
											uint8_t src_x, uint8_t src_y, uint8_t src_w, uint8_t src_h,
											uint8_t dst_x, uint8_t dst_y, uint8_t dst_w, uint8_t dst_h)
{
	if (!frame) {
		return;
	}

	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			apply_frame_locked(frame, src_x, src_y, src_w, src_h, dst_x, dst_y, dst_w, dst_h);
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		apply_frame_locked(frame, src_x, src_y, src_w, src_h, dst_x, dst_y, dst_w, dst_h);
	}
}

void m5_morsematrix_component::apply_frame_locked(const uint8_t frame[kMatrixHeight][kMatrixWidth])
{
	std::memcpy(frame_buffer_, frame, sizeof(frame_buffer_));
}

void m5_morsematrix_component::apply_frame_locked(const uint8_t frame[kMatrixHeight][kMatrixWidth],
												   uint8_t src_x, uint8_t src_y, uint8_t src_w, uint8_t src_h,
												   uint8_t dst_x, uint8_t dst_y, uint8_t dst_w, uint8_t dst_h)
{
	if (src_x >= kMatrixWidth || src_y >= kMatrixHeight || dst_x >= kMatrixWidth || dst_y >= kMatrixHeight) {
		return;
	}

	if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
		return;
	}

	uint8_t actual_src_w = src_w;
	uint8_t actual_src_h = src_h;

	if (src_x + actual_src_w > kMatrixWidth) {
		actual_src_w = static_cast<uint8_t>(kMatrixWidth - src_x);
	}
	if (src_y + actual_src_h > kMatrixHeight) {
		actual_src_h = static_cast<uint8_t>(kMatrixHeight - src_y);
	}

	uint8_t actual_dst_w = dst_w;
	uint8_t actual_dst_h = dst_h;

	if (dst_x + actual_dst_w > kMatrixWidth) {
		actual_dst_w = static_cast<uint8_t>(kMatrixWidth - dst_x);
	}
	if (dst_y + actual_dst_h > kMatrixHeight) {
		actual_dst_h = static_cast<uint8_t>(kMatrixHeight - dst_y);
	}

	uint8_t copy_w = (actual_src_w < actual_dst_w) ? actual_src_w : actual_dst_w;
	uint8_t copy_h = (actual_src_h < actual_dst_h) ? actual_src_h : actual_dst_h;

	if (src_w != dst_w || src_h != dst_h) {
		for (uint8_t dst_row = 0; dst_row < copy_h; ++dst_row) {
			for (uint8_t dst_col = 0; dst_col < copy_w; ++dst_col) {
				uint8_t src_row = static_cast<uint8_t>((static_cast<uint16_t>(dst_row) * actual_src_h) / actual_dst_h);
				uint8_t src_col = static_cast<uint8_t>((static_cast<uint16_t>(dst_col) * actual_src_w) / actual_dst_w);

				frame_buffer_[dst_y + dst_row][dst_x + dst_col] = frame[src_y + src_row][src_x + src_col];
			}
		}
	} else {
		for (uint8_t row = 0; row < copy_h; ++row) {
			for (uint8_t col = 0; col < copy_w; ++col) {
				frame_buffer_[dst_y + row][dst_x + col] = frame[src_y + row][src_x + col];
			}
		}
	}
}

void m5_morsematrix_component::set_pixel_brightness_locked(uint8_t x, uint8_t y, uint8_t level)
{
	if (x >= kMatrixWidth || y >= kMatrixHeight) {
		return;
	}

	if (level >= kBrightnessLevelCount) {
		level = static_cast<uint8_t>(kBrightnessLevelCount - 1);
	}

	frame_buffer_[y][x] = level;
}

uint8_t m5_morsematrix_component::resolve_brightness_level(uint32_t on_time_us) const
{
	if (on_time_us == 0) {
		return default_brightness_level_;
	}

	uint8_t closest_level = 0;
	uint32_t best_diff = std::numeric_limits<uint32_t>::max();

	for (uint8_t i = 0; i < kBrightnessLevelCount; ++i) {
		uint32_t current_on = compute_slot_on_time(i);
		uint32_t diff = (current_on > on_time_us) ? (current_on - on_time_us) : (on_time_us - current_on);
		if (diff < best_diff) {
			best_diff = diff;
			closest_level = i;
		}
	}

	return closest_level;
}

uint32_t m5_morsematrix_component::compute_slot_on_time(uint8_t level)
{
	if (level >= kBrightnessLevelCount) {
		level = static_cast<uint8_t>(kBrightnessLevelCount - 1);
	}

	const brightness_level_t& br = kBrightnessLevels[level];
	const uint32_t period_denominator = br.on_time_us + br.off_time_us;
	uint32_t on_time = 0;

	if (period_denominator == 0) {
		on_time = (br.on_time_us > 0) ? kSlotBaseUs : 0;
	} else {
		on_time = static_cast<uint32_t>(((static_cast<uint64_t>(kSlotBaseUs) * br.on_time_us) + (period_denominator / 2)) / period_denominator);
	}

	if (on_time > kSlotBaseUs) {
		on_time = kSlotBaseUs;
	}

	return on_time;
}

void m5_morsematrix_component::set_pixel_brightness(uint8_t x, uint8_t y, uint8_t level)
{
	if (x >= kMatrixWidth || y >= kMatrixHeight) {
		return;
	}

	if (level >= kBrightnessLevelCount) {
		level = static_cast<uint8_t>(kBrightnessLevelCount - 1);
	}

	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			set_pixel_brightness_locked(x, y, level);
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		set_pixel_brightness_locked(x, y, level);
	}
}

void m5_morsematrix_component::set_pixel_brightness_custom(uint8_t x, uint8_t y, uint32_t on_time_us, uint32_t off_time_us)
{
	if (x >= kMatrixWidth || y >= kMatrixHeight) {
		return;
	}

	// 归一化到 kSlotBaseUs 周期，确保占空比一致
	uint64_t total = static_cast<uint64_t>(on_time_us) + static_cast<uint64_t>(off_time_us);
	uint32_t normalized_on = on_time_us;
	uint32_t normalized_off = off_time_us;
	
	if (total > 0 && total != kSlotBaseUs) {
		// 按比例缩放到 kSlotBaseUs
		normalized_on = static_cast<uint32_t>(((static_cast<uint64_t>(kSlotBaseUs) * on_time_us) + (total / 2)) / total);
		if (normalized_on > kSlotBaseUs) {
			normalized_on = kSlotBaseUs;
		}
		normalized_off = kSlotBaseUs - normalized_on;
	}

	// 写入归一化后的PWM缓冲区，支持任意亮度时序
	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			custom_pwm_buffer_[y][x].on_time_us = normalized_on;
			custom_pwm_buffer_[y][x].off_time_us = normalized_off;
			use_custom_pwm_[y][x] = true;
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		custom_pwm_buffer_[y][x].on_time_us = normalized_on;
		custom_pwm_buffer_[y][x].off_time_us = normalized_off;
		use_custom_pwm_[y][x] = true;
	}
}

void m5_morsematrix_component::fill_matrix(uint8_t level)
{
	if (level >= kBrightnessLevelCount) {
		level = static_cast<uint8_t>(kBrightnessLevelCount - 1);
	}

	uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
	for (uint8_t y = 0; y < kMatrixHeight; ++y) {
		for (uint8_t x = 0; x < kMatrixWidth; ++x) {
			frame[y][x] = level;
		}
	}

	apply_frame(frame);
}

void m5_morsematrix_component::set_default_brightness_level(uint8_t level)
{
	if (level >= kBrightnessLevelCount) {
		level = static_cast<uint8_t>(kBrightnessLevelCount - 1);
	}

	default_brightness_level_ = level;
}

void m5_morsematrix_component::light_led(uint8_t x, uint8_t y)
{
	if (x >= kMatrixWidth || y >= kMatrixHeight) {
		return;
	}

	uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
	frame[y][x] = default_brightness_level_;
	apply_frame(frame);
}

void m5_morsematrix_component::light_led_by_index(uint8_t index)
{
	if (index >= kMatrixLedCount) {
		return;
	}

	uint8_t y = static_cast<uint8_t>(index / kMatrixWidth);
	uint8_t x = static_cast<uint8_t>(index % kMatrixWidth);
	light_led(x, y);
}

void m5_morsematrix_component::clear_all()
{
	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			std::memset(frame_buffer_, 0, sizeof(frame_buffer_));
			std::memset(use_custom_pwm_, 0, sizeof(use_custom_pwm_));  // 清除自定义PWM标记
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		std::memset(frame_buffer_, 0, sizeof(frame_buffer_));
		std::memset(use_custom_pwm_, 0, sizeof(use_custom_pwm_));  // 清除自定义PWM标记
	}

	set_all_pins_highz();
}

void m5_morsematrix_component::test_scan_all_leds(scan_mode_t mode, uint32_t delay_ms)
{
	ESP_LOGI(TAG, "Start scanning all LEDs, mode=%d, delay=%lums", mode, static_cast<unsigned long>(delay_ms));

	TickType_t wait_ticks = (delay_ms > 0) ? pdMS_TO_TICKS(delay_ms) : pdMS_TO_TICKS(25);
	if (wait_ticks == 0) {
		wait_ticks = 1;
	}

	auto wait = [&]() {
		vTaskDelay(wait_ticks);
	};

	bool should_clear = true;

	switch (mode) {
		case SCAN_MODE_SEQUENTIAL: {
			ESP_LOGI(TAG, "Sequential scan mode - delay per LED");
			for (uint8_t y = 0; y < kMatrixHeight; ++y) {
				for (uint8_t x = 0; x < kMatrixWidth; ++x) {
					uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
					frame[y][x] = default_brightness_level_;
					apply_frame(frame);
					wait();
				}
			}
			break;
		}

		case SCAN_MODE_ROW_BY_ROW: {
			ESP_LOGI(TAG, "Row by row scan mode - delay per row");
			for (uint8_t y = 0; y < kMatrixHeight; ++y) {
				uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
				for (uint8_t x = 0; x < kMatrixWidth; ++x) {
					frame[y][x] = default_brightness_level_;
				}
				apply_frame(frame);
				wait();
			}
			break;
		}

		case SCAN_MODE_COL_BY_COL: {
			ESP_LOGI(TAG, "Column by column scan mode - delay per column");
			for (uint8_t x = 0; x < kMatrixWidth; ++x) {
				uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
				for (uint8_t y = 0; y < kMatrixHeight; ++y) {
					frame[y][x] = default_brightness_level_;
				}
				apply_frame(frame);
				wait();
			}
			break;
		}

		case SCAN_MODE_DIAGONAL_45: {
			ESP_LOGI(TAG, "45 degree diagonal scan mode - delay per diagonal");
			for (int sum = 0; sum <= 14; ++sum) {
				uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
				for (uint8_t y = 0; y < kMatrixHeight; ++y) {
					int x = sum - y;
					if (x >= 0 && x < kMatrixWidth) {
						frame[y][x] = default_brightness_level_;
					}
				}
				apply_frame(frame);
				wait();
			}
			break;
		}

		case SCAN_MODE_DIAGONAL_135: {
			ESP_LOGI(TAG, "135 degree diagonal scan mode - delay per diagonal");
			for (int diff = -7; diff <= 7; ++diff) {
				uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
				for (uint8_t x = 0; x < kMatrixWidth; ++x) {
					int y = x - diff;
					if (y >= 0 && y < kMatrixHeight) {
						frame[y][x] = default_brightness_level_;
					}
				}
				apply_frame(frame);
				wait();
			}
			break;
		}

		case SCAN_MODE_DIAGONAL_225: {
			ESP_LOGI(TAG, "225 degree diagonal scan mode - reverse of 45 degree");
			for (int sum = 14; sum >= 0; --sum) {
				uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
				for (int y = static_cast<int>(kMatrixHeight) - 1; y >= 0; --y) {
					int x = sum - y;
					if (x >= 0 && x < kMatrixWidth) {
						frame[y][x] = default_brightness_level_;
					}
				}
				apply_frame(frame);
				wait();
			}
			break;
		}

		case SCAN_MODE_DIAGONAL_315: {
			ESP_LOGI(TAG, "315 degree diagonal scan mode - reverse of 135 degree");
			for (int diff = 7; diff >= -7; --diff) {
				uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
				for (int x = static_cast<int>(kMatrixWidth) - 1; x >= 0; --x) {
					int y = x - diff;
					if (y >= 0 && y < kMatrixHeight) {
						frame[y][x] = default_brightness_level_;
					}
				}
				apply_frame(frame);
				wait();
			}
			break;
		}

		case SCAN_MODE_SPIRAL_OUTWARD: {
			ESP_LOGI(TAG, "Spiral outward scan mode - delay per layer");
			for (int layer = 0; layer < 4; ++layer) {
				int start = 3 - layer;
				int end = 4 + layer;
				uint8_t frame[kMatrixHeight][kMatrixWidth] = {};

				for (int x = start; x <= end; ++x) {
					if (start >= 0 && start < kMatrixHeight && x >= 0 && x < kMatrixWidth) {
						frame[start][x] = default_brightness_level_;
					}
				}
				for (int y = start + 1; y <= end; ++y) {
					if (end >= 0 && end < kMatrixWidth && y >= 0 && y < kMatrixHeight) {
						frame[y][end] = default_brightness_level_;
					}
				}
				if (end > start) {
					for (int x = end - 1; x >= start; --x) {
						if (end >= 0 && end < kMatrixHeight && x >= 0 && x < kMatrixWidth) {
							frame[end][x] = default_brightness_level_;
						}
					}
					for (int y = end - 1; y > start; --y) {
						if (start >= 0 && start < kMatrixWidth && y >= 0 && y < kMatrixHeight) {
							frame[y][start] = default_brightness_level_;
						}
					}
				}

				apply_frame(frame);
				wait();
			}
			break;
		}

		case SCAN_MODE_SPIRAL_INWARD: {
			ESP_LOGI(TAG, "Spiral inward scan mode - delay per layer");
			for (int layer = 0; layer < 4; ++layer) {
				int start = layer;
				int end = static_cast<int>(kMatrixWidth) - 1 - layer;
				uint8_t frame[kMatrixHeight][kMatrixWidth] = {};

				for (int x = start; x <= end; ++x) {
					if (start >= 0 && start < kMatrixHeight && x >= 0 && x < kMatrixWidth) {
						frame[start][x] = default_brightness_level_;
					}
				}
				for (int y = start + 1; y <= end; ++y) {
					if (end >= 0 && end < kMatrixWidth && y >= 0 && y < kMatrixHeight) {
						frame[y][end] = default_brightness_level_;
					}
				}
				if (end > start) {
					for (int x = end - 1; x >= start; --x) {
						if (end >= 0 && end < kMatrixHeight && x >= 0 && x < kMatrixWidth) {
							frame[end][x] = default_brightness_level_;
						}
					}
					for (int y = end - 1; y > start; --y) {
						if (start >= 0 && start < kMatrixWidth && y >= 0 && y < kMatrixHeight) {
							frame[y][start] = default_brightness_level_;
						}
					}
				}

				apply_frame(frame);
				wait();
			}
			break;
		}

		case SCAN_MODE_ALL_ON: {
			ESP_LOGI(TAG, "All-on scan mode - delay per frame");
			uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
			for (uint8_t y = 0; y < kMatrixHeight; ++y) {
				for (uint8_t x = 0; x < kMatrixWidth; ++x) {
					frame[y][x] = default_brightness_level_;
				}
			}
			apply_frame(frame);
			wait();
			break;
		}

		case SCAN_MODE_TWINKLE: {
			ESP_LOGI(TAG, "Twinkle scan mode - 5 blinks");
			const uint8_t blink_cycles = 5;
			for (uint8_t cycle = 0; cycle < blink_cycles; ++cycle) {
				for (uint8_t parity = 0; parity < 2; ++parity) {
					uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
					for (uint8_t y = 0; y < kMatrixHeight; ++y) {
						for (uint8_t x = 0; x < kMatrixWidth; ++x) {
							if (((x + y) & 0x1) == parity) {
								frame[y][x] = default_brightness_level_;
							}
						}
					}
					apply_frame(frame);
					wait();
				}
			}
			break;
		}

		case SCAN_MODE_BRIGHTNESS_GRADIENT: {
			ESP_LOGI(TAG, "Brightness gradient scan mode - row levels");
			show_row_brightness_gradient(delay_ms);
			should_clear = (delay_ms != 0);
			break;
		}

		default:
			ESP_LOGW(TAG, "Unknown scan mode: %d", mode);
			break;
	}

	if (should_clear) {
		clear_all();
		ESP_LOGI(TAG, "Scan complete!");
	} else {
		ESP_LOGI(TAG, "Scan done, frame held active.");
	}
}

bool m5_morsematrix_component::decode_utf8(const char** utf8_str, uint32_t* codepoint)
{
	if (!utf8_str || !*utf8_str || !**utf8_str || !codepoint) {
		return false;
	}

	const uint8_t* bytes = reinterpret_cast<const uint8_t*>(*utf8_str);

	if (bytes[0] < 0x80) {
		*codepoint = bytes[0];
		*utf8_str += 1;
		return true;
	} else if ((bytes[0] & 0xE0) == 0xC0) {
		if ((bytes[1] & 0xC0) != 0x80) {
			return false;
		}
		*codepoint = ((bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F);
		*utf8_str += 2;
		return true;
	} else if ((bytes[0] & 0xF0) == 0xE0) {
		if ((bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80) {
			return false;
		}
		*codepoint = ((bytes[0] & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
		*utf8_str += 3;
		return true;
	} else if ((bytes[0] & 0xF8) == 0xF0) {
		if ((bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (bytes[3] & 0xC0) != 0x80) {
			return false;
		}
		*codepoint = ((bytes[0] & 0x07) << 18) | ((bytes[1] & 0x3F) << 12) | ((bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F);
		*utf8_str += 4;
		return true;
	}

	return false;
}

const m5_morsematrix_glyph_t* m5_morsematrix_component::find_glyph(const m5_morsematrix_font_t* font, uint32_t codepoint)
{
	if (!font || !font->glyphs) {
		return nullptr;
	}

	size_t left = 0;
	size_t right = font->glyph_count;

	while (left < right) {
		size_t mid = (left + right) / 2;
		if (font->glyphs[mid].codepoint == codepoint) {
			return &font->glyphs[mid];
		}
		if (font->glyphs[mid].codepoint < codepoint) {
			left = mid + 1;
		} else {
			right = mid;
		}
	}

	return nullptr;
}

void m5_morsematrix_component::draw_char(uint8_t x, uint8_t y, char ch, const m5_morsematrix_font_t* font, uint32_t duration_ms, uint8_t brightness_level)
{
	if (!font) {
		font = &g_m5_morsematrix_font_normal_8x8;
	}

	const m5_morsematrix_glyph_t* glyph = find_glyph(font, static_cast<uint32_t>(ch));
	if (!glyph || !glyph->bitmap) {
		ESP_LOGW(TAG, "Character '%c' (0x%02X) not found in font '%s'", ch, static_cast<uint8_t>(ch), font->name);
		return;
	}

	if (glyph->width == 0 || glyph->height == 0) {
		ESP_LOGW(TAG, "Glyph for '%c' has empty dimensions in font '%s'", ch, font->name);
		return;
	}

	if (brightness_level == 0xFF) {
		brightness_level = default_brightness_level_;
	} else if (brightness_level >= kBrightnessLevelCount) {
		ESP_LOGW(TAG, "Brightness level %u out of range, using default %u", brightness_level, default_brightness_level_);
		brightness_level = default_brightness_level_;
	}

	uint8_t frame[kMatrixHeight][kMatrixWidth] = {};

	const bool adjust_columns = (font->default_width <= kMatrixWidth && font->default_width <= 6);
	uint8_t min_col = 0;
	uint8_t max_col = (glyph->width > 0) ? static_cast<uint8_t>(glyph->width - 1) : 0;
	bool has_pixel = false;

	if (adjust_columns) {
		min_col = glyph->width;
		max_col = 0;
	}

	for (uint8_t row = 0; row < glyph->height; ++row) {
		const uint8_t* bitmap_row = glyph->bitmap + row * glyph->stride;
		for (uint8_t col = 0; col < glyph->width; ++col) {
			const uint8_t byte_idx = static_cast<uint8_t>(col / 8);
			const uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));
			if (bitmap_row[byte_idx] & (1u << bit_idx)) {
				has_pixel = true;
				if (adjust_columns) {
					if (col < min_col) {
						min_col = col;
					}
					if (col > max_col) {
						max_col = col;
					}
				}
			}
		}
	}

	if (!has_pixel) {
		apply_frame(frame);
		if (duration_ms > 0) {
			TickType_t ticks = pdMS_TO_TICKS(duration_ms);
			if (ticks == 0) {
				ticks = 1;
			}
			vTaskDelay(ticks);
			clear_all();
		}
		return;
	}

	if (!adjust_columns) {
		min_col = 0;
		max_col = (glyph->width > 0) ? static_cast<uint8_t>(glyph->width - 1) : 0;
	}

	uint8_t trimmed_width = static_cast<uint8_t>(max_col - min_col + 1);
	uint8_t leading_pad = 0;
	if (adjust_columns && trimmed_width < glyph->width) {
		leading_pad = static_cast<uint8_t>((glyph->width - trimmed_width) / 2);
	}

	uint8_t char_copy_width = glyph->width;
	uint8_t char_height = glyph->height;

	for (uint8_t row = 0; row < glyph->height; ++row) {
		const uint8_t* bitmap_row = glyph->bitmap + row * glyph->stride;
		for (uint8_t col = 0; col < glyph->width; ++col) {
			const uint8_t byte_idx = static_cast<uint8_t>(col / 8);
			const uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));

			if ((bitmap_row[byte_idx] & (1u << bit_idx)) == 0) {
				continue;
			}

			uint8_t frame_col = adjust_columns
								   ? static_cast<uint8_t>(leading_pad + (col - min_col))
								   : col;

			if (frame_col >= kMatrixWidth || row >= kMatrixHeight) {
				continue;
			}

			frame[row][frame_col] = brightness_level;
		}
	}

	apply_frame(frame, 0, 0, char_copy_width, char_height, x, y, char_copy_width, char_height);

	if (duration_ms > 0) {
		TickType_t ticks = pdMS_TO_TICKS(duration_ms);
		if (ticks == 0) {
			ticks = 1;
		}
		vTaskDelay(ticks);

		uint8_t clear_frame[kMatrixHeight][kMatrixWidth] = {};
		apply_frame(clear_frame, 0, 0, char_copy_width, char_height, x, y, char_copy_width, char_height);
	}
}

void m5_morsematrix_component::draw_char_with_animation(uint8_t x, uint8_t y, char ch,
														 char_animation_t animation,
														 const m5_morsematrix_font_t* font,
														 uint32_t duration_ms,
														 uint32_t animation_speed_ms,
														 uint8_t brightness_level)
{
	if (!font) {
		font = &g_m5_morsematrix_font_normal_8x8;
	}

	if (animation == CHAR_ANIM_NONE) {
		draw_char(x, y, ch, font, duration_ms, brightness_level);
		return;
	}

	const m5_morsematrix_glyph_t* glyph = find_glyph(font, static_cast<uint32_t>(ch));
	if (!glyph || !glyph->bitmap) {
		ESP_LOGW(TAG, "Character '%c' (0x%02X) not found in font '%s'", ch, static_cast<uint8_t>(ch), font->name);
		return;
	}

	if (glyph->width == 0 || glyph->height == 0) {
		ESP_LOGW(TAG, "Glyph for '%c' has empty dimensions in font '%s'", ch, font->name);
		return;
	}

	if (brightness_level == 0xFF) {
		brightness_level = default_brightness_level_;
	} else if (brightness_level >= kBrightnessLevelCount) {
		brightness_level = default_brightness_level_;
	}

	uint8_t char_frame[kMatrixHeight][kMatrixWidth] = {};
	uint8_t clear_frame[kMatrixHeight][kMatrixWidth] = {};

	const bool adjust_columns = (font->default_width <= kMatrixWidth && font->default_width <= 6);
	bool has_pixel = false;
	uint8_t trimmed_min_col = glyph->width;
	uint8_t trimmed_max_col = 0;

	for (uint8_t row = 0; row < glyph->height; ++row) {
		const uint8_t* bitmap_row = glyph->bitmap + row * glyph->stride;
		for (uint8_t col = 0; col < glyph->width; ++col) {
			const uint8_t byte_idx = static_cast<uint8_t>(col / 8);
			const uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));
			if (bitmap_row[byte_idx] & (1u << bit_idx)) {
				has_pixel = true;
				if (col < trimmed_min_col) {
					trimmed_min_col = col;
				}
				if (col > trimmed_max_col) {
					trimmed_max_col = col;
				}
			}
		}
	}

	if (!has_pixel) {
		return;
	}

	uint8_t min_col = adjust_columns ? trimmed_min_col : 0;
	uint8_t max_col = adjust_columns ? trimmed_max_col : (glyph->width > 0 ? static_cast<uint8_t>(glyph->width - 1) : 0);
	uint8_t trimmed_width = static_cast<uint8_t>(max_col - min_col + 1);
	uint8_t leading_pad = (adjust_columns && trimmed_width < glyph->width)
							  ? static_cast<uint8_t>((glyph->width - trimmed_width) / 2)
							  : 0;

	for (uint8_t row = 0; row < glyph->height; ++row) {
		const uint8_t* bitmap_row = glyph->bitmap + row * glyph->stride;
		for (uint8_t col = 0; col < glyph->width; ++col) {
			const uint8_t byte_idx = static_cast<uint8_t>(col / 8);
			const uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));
			if ((bitmap_row[byte_idx] & (1u << bit_idx)) == 0) {
				continue;
			}

			uint8_t frame_col = adjust_columns
									 ? static_cast<uint8_t>(leading_pad + (col - min_col))
									 : col;

			if (frame_col < kMatrixWidth && row < kMatrixHeight) {
				char_frame[row][frame_col] = brightness_level;
			}
		}
	}

	uint8_t char_visible_width = adjust_columns ? trimmed_width : glyph->width;
	uint8_t char_copy_width = glyph->width;
	uint8_t char_height = glyph->height;
	uint8_t char_data_start_x = adjust_columns ? leading_pad : 0;

	TickType_t anim_ticks = pdMS_TO_TICKS(animation_speed_ms);
	if (anim_ticks == 0) {
		anim_ticks = 1;
	}

	auto clear_region = [&](uint8_t width, uint8_t height, uint8_t target_x, uint8_t target_y) {
		if (width == 0 || height == 0) {
			return;
		}
		if (target_x >= kMatrixWidth || target_y >= kMatrixHeight) {
			return;
		}

		uint8_t actual_width = width;
		uint8_t actual_height = height;

		if (target_x + actual_width > kMatrixWidth) {
			actual_width = static_cast<uint8_t>(kMatrixWidth - target_x);
		}
		if (target_y + actual_height > kMatrixHeight) {
			actual_height = static_cast<uint8_t>(kMatrixHeight - target_y);
		}

		if (actual_width == 0 || actual_height == 0) {
			return;
		}

		apply_frame(clear_frame, 0, 0, actual_width, actual_height, target_x, target_y, actual_width, actual_height);
	};

	auto draw_partial = [&](const uint8_t image[kMatrixHeight][kMatrixWidth],
							uint8_t img_start_x,
							uint8_t width,
							uint8_t height,
							int16_t dst_x,
							uint8_t dst_y,
							uint8_t bound_x,
							uint8_t bound_width) {
		if (width == 0 || height == 0) {
			return;
		}

		if (bound_width == 0) {
			return;
		}

		int16_t clip_start = dst_x;
		int16_t clip_end = static_cast<int16_t>(dst_x) + static_cast<int16_t>(width);

		int16_t bound_start = static_cast<int16_t>(bound_x);
		int16_t bound_end = bound_start + static_cast<int16_t>(bound_width);

		if (clip_start < bound_start) {
			clip_start = bound_start;
		}
		if (clip_end > bound_end) {
			clip_end = bound_end;
		}

		if (clip_start < 0) {
			clip_start = 0;
		}
		if (clip_end > static_cast<int16_t>(kMatrixWidth)) {
			clip_end = static_cast<int16_t>(kMatrixWidth);
		}

		if (clip_end <= clip_start) {
			return;
		}

		uint8_t src_offset = static_cast<uint8_t>(clip_start - dst_x);
		uint8_t effective_width = static_cast<uint8_t>(clip_end - clip_start);

		apply_frame(image,
					static_cast<uint8_t>(img_start_x + src_offset),
					0,
					effective_width,
					height,
					static_cast<uint8_t>(clip_start),
					dst_y,
					effective_width,
					height);
	};

	switch (animation) {
		case CHAR_ANIM_SLIDE_LEFT_IN_RIGHT_OUT: {
			if (last_char_ != '\0' && last_char_x_ == x && last_char_y_ == y &&
				last_char_width_ > 0 && last_char_height_ > 0) {
				uint8_t prev_slide_start_x = last_char_x_;
				uint8_t prev_slide_width = last_char_width_;

				for (int16_t draw_x = static_cast<int16_t>(last_char_x_);
					 draw_x <= static_cast<int16_t>(last_char_x_) + static_cast<int16_t>(last_char_width_);
					 ++draw_x) {
					clear_region(prev_slide_width, last_char_height_, prev_slide_start_x, last_char_y_);
					draw_partial(last_char_frame_, 0, last_char_width_, last_char_height_, draw_x, last_char_y_, last_char_x_, last_char_width_);
					vTaskDelay(anim_ticks);
				}
				clear_region(prev_slide_width, last_char_height_, prev_slide_start_x, last_char_y_);
			} else {
				clear_region(char_copy_width, char_height, x, y);
			}

			uint8_t slide_start_x = x;
			uint8_t slide_width = char_copy_width;

			for (int16_t draw_x = static_cast<int16_t>(x) - static_cast<int16_t>(char_copy_width);
				 draw_x <= static_cast<int16_t>(x);
				 ++draw_x) {
				clear_region(slide_width, char_height, slide_start_x, y);
				draw_partial(char_frame, 0, char_copy_width, char_height, draw_x, y, x, char_copy_width);
				vTaskDelay(anim_ticks);
			}
			break;
		}

		case CHAR_ANIM_RANDOM_FILL: {
			clear_region(char_copy_width, char_height, x, y);

			struct Pixel {
				uint8_t x;
				uint8_t y;
			};

			Pixel pixels[64];
			uint8_t pixel_count = 0;

			for (uint8_t row = 0; row < char_height; ++row) {
				for (uint8_t col = 0; col < char_visible_width; ++col) {
					if (char_frame[row][char_data_start_x + col] > 0) {
						pixels[pixel_count++] = {col, row};
					}
				}
			}

			for (uint8_t i = pixel_count; i > 1; --i) {
				uint8_t j = static_cast<uint8_t>(esp_timer_get_time() % i);
				Pixel tmp = pixels[i - 1];
				pixels[i - 1] = pixels[j];
				pixels[j] = tmp;
			}

			uint8_t temp_frame[kMatrixHeight][kMatrixWidth] = {};
			for (uint8_t i = 0; i < pixel_count; ++i) {
				temp_frame[pixels[i].y][char_data_start_x + pixels[i].x] = brightness_level;
				apply_frame(temp_frame, 0, 0, char_copy_width, char_height, x, y, char_copy_width, char_height);
				vTaskDelay(anim_ticks);
			}
			break;
		}

		case CHAR_ANIM_FADE_IN: {
			// 逐渐亮起效果：从亮度0逐步增加到目标亮度
			clear_region(char_copy_width, char_height, x, y);

			// 从亮度1逐渐增加到目标亮度
			for (uint8_t level = 1; level <= brightness_level; ++level) {
				uint8_t temp_frame[kMatrixHeight][kMatrixWidth] = {};
				
				// 复制字符形状，但使用当前亮度等级
				for (uint8_t row = 0; row < char_height; ++row) {
					for (uint8_t col = 0; col < char_copy_width; ++col) {
						if (char_frame[row][col] > 0) {
							temp_frame[row][col] = level;
						}
					}
				}
				
				apply_frame(temp_frame, 0, 0, char_copy_width, char_height, x, y, char_copy_width, char_height);
				vTaskDelay(anim_ticks);
			}
			break;
		}

		default:
			break;
	}

	draw_partial(char_frame, 0, char_copy_width, char_height, static_cast<int16_t>(x), y, x, char_copy_width);

	last_char_ = ch;
	last_char_x_ = x;
	last_char_y_ = y;
	last_char_width_ = char_copy_width;
	last_char_height_ = char_height;
	last_char_start_x_ = char_data_start_x;
	std::memcpy(last_char_frame_, char_frame, sizeof(last_char_frame_));

	if (duration_ms > 0) {
		TickType_t ticks = pdMS_TO_TICKS(duration_ms);
		if (ticks == 0) {
			ticks = 1;
		}
		vTaskDelay(ticks);

		clear_region(char_copy_width, char_height, x, y);

		last_char_ = '\0';
		last_char_width_ = 0;
		last_char_height_ = 0;
		last_char_start_x_ = 0;
	}
}

int m5_morsematrix_component::draw_text(uint8_t x, uint8_t y, const char* utf8_str, const m5_morsematrix_font_t* font, uint32_t duration_ms)
{
	if (!utf8_str) {
		return 0;
	}

	if (!font) {
		font = &g_m5_morsematrix_font_normal_8x8;
	}

	struct GlyphPosition {
		const m5_morsematrix_glyph_t* glyph;
		uint8_t x;
		uint8_t y;
	};

	GlyphPosition glyph_positions[32];
	int char_count = 0;
	uint8_t cursor_x = x;
	const char* str_ptr = utf8_str;

	while (*str_ptr && char_count < 32) {
		uint32_t codepoint = 0;
		if (!decode_utf8(&str_ptr, &codepoint)) {
			ESP_LOGW(TAG, "Invalid UTF-8 sequence at position %d", char_count);
			break;
		}

		const m5_morsematrix_glyph_t* glyph = find_glyph(font, codepoint);
		if (!glyph) {
			ESP_LOGW(TAG, "Codepoint U+%04X not found in font '%s'", codepoint, font->name);
			cursor_x += font->default_width;
			continue;
		}

		if (cursor_x >= kMatrixWidth) {
			break;
		}

		glyph_positions[char_count].glyph = glyph;
		glyph_positions[char_count].x = cursor_x;
		glyph_positions[char_count].y = y;

		cursor_x += glyph->width;
		char_count++;
	}

	if (char_count == 0) {
		return 0;
	}

	uint8_t frame[kMatrixHeight][kMatrixWidth] = {};

	for (int i = 0; i < char_count; ++i) {
		const GlyphPosition& gp = glyph_positions[i];
		for (uint8_t row = 0; row < gp.glyph->height && (gp.y + row) < kMatrixHeight; ++row) {
			for (uint8_t col = 0; col < gp.glyph->width && (gp.x + col) < kMatrixWidth; ++col) {
				uint8_t byte_idx = static_cast<uint8_t>(col / 8);
				uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));
				const uint8_t* bitmap_row = gp.glyph->bitmap + row * gp.glyph->stride;

				if (bitmap_row[byte_idx] & (1u << bit_idx)) {
					frame[gp.y + row][gp.x + col] = default_brightness_level_;
				}
			}
		}
	}

	apply_frame(frame);

	if (duration_ms > 0) {
		TickType_t ticks = pdMS_TO_TICKS(duration_ms);
		if (ticks == 0) {
			ticks = 1;
		}
		vTaskDelay(ticks);
		clear_all();
	}

	return char_count;
}

void m5_morsematrix_component::draw_text_with_animation(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2,
														 const char* text,
														 text_animation_t animation,
														 const m5_morsematrix_font_t* font,
														 uint32_t char_delay_ms,
														 uint32_t animation_speed_ms,
														 uint8_t brightness_level)
{
	if (!text || text[0] == '\0') {
		return;
	}

	if (!font) {
		font = &g_m5_morsematrix_font_normal_8x8;
	}

	if (brightness_level == 0xFF) {
		brightness_level = default_brightness_level_;
	} else if (brightness_level >= kBrightnessLevelCount) {
		brightness_level = default_brightness_level_;
	}

	uint8_t region_width = (x2 >= x1) ? static_cast<uint8_t>(x2 - x1 + 1) : 1;
	uint8_t region_height = (y2 >= y1) ? static_cast<uint8_t>(y2 - y1 + 1) : 1;

	size_t text_len = std::strlen(text);
	TickType_t anim_ticks = pdMS_TO_TICKS(animation_speed_ms);
	if (anim_ticks == 0) {
		anim_ticks = 1;
	}

	auto clear_region = [this](uint8_t width, uint8_t height, uint8_t target_x, uint8_t target_y) {
		if (width == 0 || height == 0) {
			return;
		}
		if (target_x >= kMatrixWidth || target_y >= kMatrixHeight) {
			return;
		}

		for (uint8_t row = 0; row < height && (target_y + row) < kMatrixHeight; ++row) {
			for (uint8_t col = 0; col < width && (target_x + col) < kMatrixWidth; ++col) {
				set_pixel_brightness(target_x + col, target_y + row, 0);
			}
		}
	};

	struct CharGlyph {
		const m5_morsematrix_glyph_t* glyph;
		char ch;
	};

	CharGlyph char_glyphs[64];
	size_t valid_char_count = 0; // 最多缓存64个可绘制字符，防止超出堆栈

	for (size_t i = 0; i < text_len && i < 64; ++i) {
		const m5_morsematrix_glyph_t* glyph = find_glyph(font, static_cast<uint32_t>(text[i]));
		if (glyph && glyph->bitmap) {
			char_glyphs[valid_char_count].glyph = glyph;
			char_glyphs[valid_char_count].ch = text[i];
			valid_char_count++;
		}
	}

	if (valid_char_count == 0) {
		return;
	}

	switch (animation) {
		case TEXT_ANIM_NONE: {
			for (size_t i = 0; i < valid_char_count; ++i) {
				clear_region(region_width, region_height, x1, y1);
				draw_char(x1, y1, char_glyphs[i].ch, font, 0, brightness_level);

				if (char_delay_ms > 0 && i < valid_char_count - 1) {
					vTaskDelay(pdMS_TO_TICKS(char_delay_ms));
				}
			}
			break;
		}

	case TEXT_ANIM_SLIDE_LEFT: {
		// 通过构造整条文本带并向左滚动来实现滑动效果
		// 构建连续文本帧（所有字符连接在一起，字符间间隔1列）
		const uint8_t char_spacing = 1;
		uint16_t total_width = 0;
		
		// 计算总宽度
		for (size_t i = 0; i < valid_char_count; ++i) {
			total_width += char_glyphs[i].glyph->width;
			if (i < valid_char_count - 1) {
				total_width += char_spacing;
			}
		}
		
		// 创建连续文本帧缓冲
		uint8_t* text_frame = new uint8_t[kMatrixHeight * total_width]();
		
		uint16_t current_x = 0;
		for (size_t i = 0; i < valid_char_count; ++i) {
			const m5_morsematrix_glyph_t* glyph = char_glyphs[i].glyph;
			uint8_t char_width = glyph->width;
			
			// 填充字符数据
			for (uint8_t row = 0; row < glyph->height && row < region_height; ++row) {
				const uint8_t* bitmap_row = glyph->bitmap + row * glyph->stride;
				for (uint8_t col = 0; col < char_width; ++col) {
					const uint8_t byte_idx = static_cast<uint8_t>(col / 8);
					const uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));
					if (bitmap_row[byte_idx] & (1u << bit_idx)) {
						text_frame[row * total_width + current_x + col] = 1;
					}
				}
			}
			
			current_x += char_width + char_spacing;
		}
		
		// 滚动动画：从右侧进入，完全离开左侧，再等待矩阵宽度
		int16_t total_offset = static_cast<int16_t>(region_width + total_width + region_width);
		
		for (int16_t offset = region_width; offset >= -static_cast<int16_t>(total_width + region_width); --offset) {
			clear_region(region_width, region_height, x1, y1);
			
			for (uint8_t row = 0; row < region_height && (y1 + row) < kMatrixHeight; ++row) {
				for (uint16_t col = 0; col < total_width; ++col) {
					int16_t target_col = offset + col;
					
					if (target_col >= 0 && target_col < region_width && (x1 + target_col) < kMatrixWidth) {
						if (text_frame[row * total_width + col] > 0) {
							// 计算渐入渐出亮度（边缘2列渐变）
							uint8_t fade_brightness = brightness_level;
							
							// 右侧渐入（最右2列）
							if (target_col >= static_cast<int16_t>(region_width - 2)) {
								uint8_t fade_steps = static_cast<uint8_t>(region_width - target_col);
								uint8_t gradient = static_cast<uint8_t>((fade_steps * 255) / 3);
								fade_brightness = gradient_to_brightness_level(gradient);
							}
							// 左侧渐出（最左2列）
							else if (target_col < 2) {
								uint8_t gradient = static_cast<uint8_t>((target_col + 1) * 255 / 3);
								fade_brightness = gradient_to_brightness_level(gradient);
							}
							
							set_pixel_brightness(x1 + target_col, y1 + row, fade_brightness);
						}
					}
				}
			}
			
			vTaskDelay(anim_ticks);
		}
		
		delete[] text_frame;
		break;
	}		case TEXT_ANIM_FADE_IN_OUT: {
			// 字体整体向右偏移1格
			const uint8_t x_offset = 1;
			
			for (size_t i = 0; i < valid_char_count; ++i) {
				const m5_morsematrix_glyph_t* glyph = char_glyphs[i].glyph;

				// 准备字符位图缓存（应用x偏移）
				bool pixel_mask[kMatrixHeight][kMatrixWidth] = {};
				for (uint8_t row = 0; row < glyph->height && row < region_height; ++row) {
					const uint8_t* bitmap_row = glyph->bitmap + row * glyph->stride;
					for (uint8_t col = 0; col < glyph->width; ++col) {
						uint8_t target_col = col + x_offset;
						if (target_col >= region_width) continue;
						
						const uint8_t byte_idx = static_cast<uint8_t>(col / 8);
						const uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));
						if (bitmap_row[byte_idx] & (1u << bit_idx)) {
							pixel_mask[row][target_col] = true;
						}
					}
				}

				clear_region(region_width, region_height, x1, y1);

				for (uint8_t level = 0; level <= brightness_level; ++level) {
					for (uint8_t row = 0; row < region_height && (y1 + row) < kMatrixHeight; ++row) {
						for (uint8_t col = 0; col < region_width && (x1 + col) < kMatrixWidth; ++col) {
							if (pixel_mask[row][col]) {
								set_pixel_brightness(x1 + col, y1 + row, level);
							}
						}
					}
					vTaskDelay(anim_ticks);
				}

				if (char_delay_ms > 0) {
					vTaskDelay(pdMS_TO_TICKS(char_delay_ms));
				}

				for (int8_t level = static_cast<int8_t>(brightness_level); level >= 0; --level) {
					for (uint8_t row = 0; row < region_height && (y1 + row) < kMatrixHeight; ++row) {
						for (uint8_t col = 0; col < region_width && (x1 + col) < kMatrixWidth; ++col) {
							if (pixel_mask[row][col]) {
								set_pixel_brightness(x1 + col, y1 + row, static_cast<uint8_t>(level));
							}
						}
					}
					vTaskDelay(anim_ticks);
				}
			}
			break;
		}

	case TEXT_ANIM_STREAM_ASSEMBLE: {
		// 字体整体向右偏移1格
		const uint8_t x_offset = 1;
		
		for (size_t i = 0; i < valid_char_count; ++i) {
			const m5_morsematrix_glyph_t* glyph = char_glyphs[i].glyph;
			uint8_t char_width = glyph->width;

			// 准备字符位图缓存（应用x偏移）
			uint8_t char_frame[kMatrixHeight][kMatrixWidth] = {};
			for (uint8_t row = 0; row < glyph->height && row < region_height; ++row) {
				const uint8_t* bitmap_row = glyph->bitmap + row * glyph->stride;
				for (uint8_t col = 0; col < char_width; ++col) {
					uint8_t target_col = col + x_offset;
					if (target_col >= region_width) continue;
					
					const uint8_t byte_idx = static_cast<uint8_t>(col / 8);
					const uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));
					if (bitmap_row[byte_idx] & (1u << bit_idx)) {
						char_frame[row][target_col] = brightness_level;
					}
				}
			}

			// 阶段1: 行交错插入动画（横向）
			// 奇数行(1,3,5,7) 从左边插入, 偶数行(0,2,4,6) 从右边插入
			const uint8_t insert_steps = region_width; // 8步完成插入
			
			for (uint8_t step = 0; step <= insert_steps; ++step) {
				clear_region(region_width, region_height, x1, y1);
				
				for (uint8_t row = 0; row < region_height && (y1 + row) < kMatrixHeight; ++row) {
					// 判断是奇数行还是偶数行 (y坐标)
					bool is_odd_row = (row % 2) == 1;
					
					for (uint8_t col = 0; col < region_width; ++col) {
						if (char_frame[row][col] == 0) continue;
						
						int16_t current_x;
						if (is_odd_row) {
							// 奇数行: 从左往右插入 (从-1开始)
							current_x = static_cast<int16_t>(col) - (insert_steps - step);
						} else {
							// 偶数行: 从右往左插入 (从region_width开始)
							current_x = static_cast<int16_t>(col) + (insert_steps - step);
						}
						
						// 检查X坐标是否在有效范围内
						if (current_x >= 0 && current_x < region_width) {
							uint8_t final_x = x1 + static_cast<uint8_t>(current_x);
							uint8_t final_y = y1 + row;
							if (final_x < kMatrixWidth && final_y < kMatrixHeight) {
								set_pixel_brightness(final_x, final_y, char_frame[row][col]);
							}
						}
					}
				}
				
				vTaskDelay(anim_ticks);
			}
			
			// 显示完整字符
			if (char_delay_ms > 0) {
				vTaskDelay(pdMS_TO_TICKS(char_delay_ms));
			}
			
			// 阶段2: 行交错退出动画（横向）
			// 奇数行(1,3,5,7) 向右滑出, 偶数行(0,2,4,6) 向左滑出
			const uint8_t exit_steps = region_width;
			
			for (uint8_t step = 0; step <= exit_steps; ++step) {
				clear_region(region_width, region_height, x1, y1);
				
				for (uint8_t row = 0; row < region_height && (y1 + row) < kMatrixHeight; ++row) {
					// 判断是奇数行还是偶数行
					bool is_odd_row = (row % 2) == 1;
					
					for (uint8_t col = 0; col < region_width; ++col) {
						if (char_frame[row][col] == 0) continue;
						
						int16_t current_x;
						if (is_odd_row) {
							// 奇数行: 向右滑出
							current_x = static_cast<int16_t>(col) + step;
						} else {
							// 偶数行: 向左滑出
							current_x = static_cast<int16_t>(col) - step;
						}
						
						// 检查X坐标是否在有效范围内
						if (current_x >= 0 && current_x < region_width) {
							uint8_t final_x = x1 + static_cast<uint8_t>(current_x);
							uint8_t final_y = y1 + row;
							if (final_x < kMatrixWidth && final_y < kMatrixHeight) {
								set_pixel_brightness(final_x, final_y, char_frame[row][col]);
							}
						}
					}
				}
				
				vTaskDelay(anim_ticks);
			}
		}
		break;
	}		default: {
			for (size_t i = 0; i < valid_char_count; ++i) {
				clear_region(region_width, region_height, x1, y1);
				draw_char(x1, y1, char_glyphs[i].ch, font, 0, brightness_level);

				if (char_delay_ms > 0 && i < valid_char_count - 1) {
					vTaskDelay(pdMS_TO_TICKS(char_delay_ms));
				}
			}
			break;
		}
	}
}

void m5_morsematrix_component::debug_print_glyph(char ch, const m5_morsematrix_font_t* font)
{
	if (!font) {
		font = &g_m5_morsematrix_font_normal_8x8;
	}

	const m5_morsematrix_glyph_t* glyph = find_glyph(font, static_cast<uint32_t>(ch));
	if (!glyph || !glyph->bitmap) {
		ESP_LOGW(TAG, "Character '%c' (0x%02X) not found in font '%s'", ch, static_cast<uint8_t>(ch), font->name);
		return;
	}

	ESP_LOGI(TAG, "=== Glyph for '%c' (U+%04X) ===", ch, glyph->codepoint);
	ESP_LOGI(TAG, "Size: %dx%d, Stride: %d", glyph->width, glyph->height, glyph->stride);
	ESP_LOGI(TAG, "Bitmap (X=lit, .=off):");

	for (uint8_t row = 0; row < glyph->height; ++row) {
		char line_buffer[65];
		int pos = 0;

		pos += std::snprintf(line_buffer + pos, sizeof(line_buffer) - pos, "Row%d: ", row);

		for (uint8_t col = 0; col < 8; ++col) {
			if (col < glyph->width) {
				uint8_t byte_idx = static_cast<uint8_t>(col / 8);
				uint8_t bit_idx = static_cast<uint8_t>(7 - (col % 8));

				const uint8_t* bitmap_row = glyph->bitmap + row * glyph->stride;

				if (bitmap_row[byte_idx] & (1u << bit_idx)) {
					line_buffer[pos++] = 'X';
				} else {
					line_buffer[pos++] = '.';
				}
			} else {
				line_buffer[pos++] = '.';
			}
		}
		line_buffer[pos] = '\0';
		ESP_LOGI(TAG, "%s", line_buffer);
	}

	ESP_LOGI(TAG, "=== End of glyph ===");
}

void m5_morsematrix_component::light_leds_range(uint8_t start_index, uint8_t end_index, uint32_t duration_ms)
{
	if (start_index > 63) {
		start_index = 63;
	}
	if (end_index > 63) {
		end_index = 63;
	}

	if (start_index > end_index) {
		std::swap(start_index, end_index);
	}

	ESP_LOGI(TAG, "Lighting LEDs from %d to %d (%d LEDs total) for %lums",
			 start_index, end_index, end_index - start_index + 1, static_cast<unsigned long>(duration_ms));

	uint8_t frame[kMatrixHeight][kMatrixWidth] = {};
	for (uint8_t index = start_index; index <= end_index; ++index) {
		uint8_t y = static_cast<uint8_t>(index / kMatrixWidth);
		uint8_t x = static_cast<uint8_t>(index % kMatrixWidth);
		frame[y][x] = default_brightness_level_;
	}

	if (duration_ms > 0) {
		TickType_t start_time = xTaskGetTickCount();
		TickType_t duration_ticks = pdMS_TO_TICKS(duration_ms);
		if (duration_ticks == 0) {
			duration_ticks = 1;
		}

		while ((xTaskGetTickCount() - start_time) < duration_ticks) {
			apply_frame(frame);
			vTaskDelay(pdMS_TO_TICKS(10));
		}

		clear_all();
	} else {
		apply_frame(frame);
	}
}

void m5_morsematrix_component::show_row_brightness_gradient(uint32_t hold_duration_ms)
{
	uint8_t frame[kMatrixHeight][kMatrixWidth] = {};

	for (uint8_t y = 0; y < kMatrixHeight; ++y) {
		uint8_t level = (y < kBrightnessLevelCount) ? y : static_cast<uint8_t>(kBrightnessLevelCount - 1);

		for (uint8_t x = 0; x < kMatrixWidth; ++x) {
			frame[y][x] = level;
		}
	}

	apply_frame(frame);

	if (hold_duration_ms > 0) {
		TickType_t ticks = pdMS_TO_TICKS(hold_duration_ms);
		if (ticks == 0) {
			ticks = 1;
		}
		vTaskDelay(ticks);
	}
}

bool m5_morsematrix_component::set_row_brightness_from_us(uint8_t row, uint32_t on_time_us, uint32_t off_time_us)
{
	if (row >= kMatrixHeight) {
		ESP_LOGW(TAG, "Row index out of range: %u", static_cast<unsigned>(row));
		return false;
	}

	// 如果 on_time_us 和 off_time_us 都为0，清除该行的自定义PWM
	if (on_time_us == 0 && off_time_us == 0) {
		if (frame_mutex_) {
			if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
				for (uint8_t x = 0; x < kMatrixWidth; ++x) {
					use_custom_pwm_[row][x] = false;
				}
				xSemaphoreGive(frame_mutex_);
			}
		} else {
			for (uint8_t x = 0; x < kMatrixWidth; ++x) {
				use_custom_pwm_[row][x] = false;
			}
		}

		ESP_LOGI(TAG, "Row %u custom PWM cleared (reverting to frame levels)", static_cast<unsigned>(row + 1));
		return true;
	}

	// 归一化到 kSlotBaseUs 周期，确保占空比一致
	uint64_t total = static_cast<uint64_t>(on_time_us) + static_cast<uint64_t>(off_time_us);
	uint32_t normalized_on = on_time_us;
	uint32_t normalized_off = off_time_us;
	
	if (total > 0 && total != kSlotBaseUs) {
		// 按比例缩放到 kSlotBaseUs
		normalized_on = static_cast<uint32_t>(((static_cast<uint64_t>(kSlotBaseUs) * on_time_us) + (total / 2)) / total);
		if (normalized_on > kSlotBaseUs) {
			normalized_on = kSlotBaseUs;
		}
		normalized_off = kSlotBaseUs - normalized_on;
	}

	// 对该行的所有像素设置自定义PWM
	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			for (uint8_t x = 0; x < kMatrixWidth; ++x) {
				custom_pwm_buffer_[row][x].on_time_us = normalized_on;
				custom_pwm_buffer_[row][x].off_time_us = normalized_off;
				use_custom_pwm_[row][x] = true;
			}
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		for (uint8_t x = 0; x < kMatrixWidth; ++x) {
			custom_pwm_buffer_[row][x].on_time_us = normalized_on;
			custom_pwm_buffer_[row][x].off_time_us = normalized_off;
			use_custom_pwm_[row][x] = true;
		}
	}

	ESP_LOGI(TAG, "Row %u custom PWM applied: requested on=%luus off=%luus -> normalized on=%luus off=%luus",
			 static_cast<unsigned>(row + 1),
			 static_cast<unsigned long>(on_time_us),
			 static_cast<unsigned long>(off_time_us),
			 static_cast<unsigned long>(normalized_on),
			 static_cast<unsigned long>(normalized_off));

	return true;
}

void m5_morsematrix_component::draw_morse_input_in_col(const char* morse_buffer,
													   uint8_t column,
													   uint8_t dot_level,
													   uint8_t dash_level)
{
	if (column >= kMatrixWidth) {
		return;
	}

	if (!morse_buffer) {
		morse_buffer = "";
	}

	if (dot_level >= kBrightnessLevelCount) {
		dot_level = default_brightness_level_;
	}

	if (dash_level >= kBrightnessLevelCount) {
		dash_level = default_brightness_level_;
	}

	uint8_t column_frame[kMatrixHeight][kMatrixWidth] = {};

	size_t len = std::strlen(morse_buffer);
	if (len == 0) {
		apply_frame(column_frame, 0, 0, 1, kMatrixHeight, column, 0, 1, kMatrixHeight);
		return;
	}

	size_t display_count = (len > kMatrixHeight) ? kMatrixHeight : len;
	size_t start_index = len - display_count;

	for (size_t i = 0; i < display_count; ++i) {
		char symbol = morse_buffer[start_index + i];
		uint8_t brightness = 0;

		if (symbol == '.') {
			brightness = dot_level;
		} else if (symbol == '-') {
			brightness = dash_level;
		} else {
			continue;
		}

		uint8_t target_row = static_cast<uint8_t>(i);
		column_frame[target_row][0] = brightness;
	}

	apply_frame(column_frame, 0, 0, 1, kMatrixHeight, column, 0, 1, kMatrixHeight);
}

const char* m5_morsematrix_component::decode_morse(const char* morse_code)
{
	auto callback = decode_morse_callback_;
	if (!callback) {
		return "?";
	}
	return callback(this, morse_code);
}

void m5_morsematrix_component::reset_char_animation_state()
{
	auto reset_state = [this]() {
		last_char_ = '\0';
		last_char_x_ = 0;
		last_char_y_ = 0;
		last_char_width_ = 0;
		last_char_height_ = 0;
		last_char_start_x_ = 0;
		std::memset(last_char_frame_, 0, sizeof(last_char_frame_));
	};

	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			reset_state();
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		reset_state();
	}
}

void m5_morsematrix_component::display_morse_input(const char* morse_buffer)
{
	auto callback = display_morse_input_callback_;
	if (callback) {
		callback(this, morse_buffer);
	}
}

void m5_morsematrix_component::set_display_morse_input_callback(void (*callback)(m5_morsematrix_component* self, const char* morse_buffer))
{
	auto target_callback = callback ? callback : &m5_morsematrix_component::handle_default_display_morse_input;

	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			display_morse_input_callback_ = target_callback;
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		display_morse_input_callback_ = target_callback;
	}
}

void m5_morsematrix_component::display_decoded_text(const char* text)
{
	auto callback = display_decoded_text_callback_;
	if (callback) {
		callback(this, text);
	}
}

void m5_morsematrix_component::set_display_decoded_text_callback(void (*callback)(m5_morsematrix_component* self, const char* text))
{
	auto target_callback = callback ? callback : &m5_morsematrix_component::handle_default_display_decoded_text;

	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			display_decoded_text_callback_ = target_callback;
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		display_decoded_text_callback_ = target_callback;
	}
}

void m5_morsematrix_component::set_decode_morse_callback(const char* (*callback)(m5_morsematrix_component* self, const char* morse_code))
{
	auto target_callback = callback ? callback : &m5_morsematrix_component::handle_default_decode_morse;

	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			decode_morse_callback_ = target_callback;
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		decode_morse_callback_ = target_callback;
	}
}

void m5_morsematrix_component::set_morse_display_brightness_levels(uint8_t dot_level, uint8_t dash_level)
{
	if (dot_level >= kBrightnessLevelCount) {
		dot_level = default_brightness_level_;
	}
	if (dash_level >= kBrightnessLevelCount) {
		dash_level = default_brightness_level_;
	}

	if (frame_mutex_) {
		if (xSemaphoreTake(frame_mutex_, portMAX_DELAY) == pdTRUE) {
			morse_dot_brightness_level_ = dot_level;
			morse_dash_brightness_level_ = dash_level;
			xSemaphoreGive(frame_mutex_);
		}
	} else {
		morse_dot_brightness_level_ = dot_level;
		morse_dash_brightness_level_ = dash_level;
	}
}

void m5_morsematrix_component::handle_default_display_morse_input(m5_morsematrix_component* self, const char* morse_buffer)
{
	if (!self) {
		return;
	}

	if (!morse_buffer) {
		morse_buffer = "";
	}

	self->draw_morse_input_in_col(morse_buffer,
								  static_cast<uint8_t>(kMatrixWidth - 1),
								  self->morse_dot_brightness_level_,
								  self->morse_dash_brightness_level_);
}

void m5_morsematrix_component::handle_default_display_decoded_text(m5_morsematrix_component* self, const char* text)
{
	if (!self) {
		return;
	}

	if (!text || text[0] == '\0') {
		self->clear_all();
		return;
	}

	size_t len = std::strlen(text);
	char latest = text[len - 1];

	self->clear_all();
	self->draw_char_with_animation(1, 0, latest,
								   CHAR_ANIM_SLIDE_LEFT_IN_RIGHT_OUT,
								   &g_m5_morsematrix_font_ascii_5x8,
								   0,
								   50,
								   self->morse_dash_brightness_level_);
}

uint8_t m5_morsematrix_component::update_first_column_breathing(uint8_t brightness_level)
{
	if (brightness_level >= kBrightnessLevelCount) {
		brightness_level = static_cast<uint8_t>(kBrightnessLevelCount - 1);
	}

	// 获取当前时间
	TickType_t now = xTaskGetTickCount();
	
	// 初始化或计算时间增量
	if (last_breathing_update_ == 0) {
		last_breathing_update_ = now;
	}
	
	// 计算时间差（毫秒）
	uint32_t delta_ms = pdTICKS_TO_MS(now - last_breathing_update_);
	last_breathing_update_ = now;
	
	// 呼吸周期：2000ms（2秒完成一次呼吸循环）
	// 每毫秒相位增加 360/2000 = 0.18度
	constexpr uint32_t breathing_period_ms = 2000;
	constexpr float degrees_per_ms = 360.0f / static_cast<float>(breathing_period_ms);
	
	// 更新相位（使用浮点数计算，然后转换为整数）
	float phase_increment = static_cast<float>(delta_ms) * degrees_per_ms;
	breathing_phase_ = (breathing_phase_ + static_cast<uint32_t>(phase_increment)) % 360;
	
	// 使用正弦函数计算亮度（范围 0.0 到 1.0）
	// sin函数在0-180度之间从0升到1再降到0，正好符合呼吸效果
	float phase_radians = static_cast<float>(breathing_phase_) * 3.14159265f / 180.0f;
	float brightness_ratio = (std::sin(phase_radians) + 1.0f) / 2.0f; // 将 [-1,1] 映射到 [0,1]
	
	// 映射到亮度等级（0 到 max_brightness）
	uint8_t current_brightness = static_cast<uint8_t>(brightness_ratio * static_cast<float>(brightness_level));
	
	// 设置第一列（x=0, y=0~7）的亮度
	for (uint8_t y = 0; y < kMatrixHeight; y++) {
		set_pixel_brightness(0, y, current_brightness);
	}
	
	return current_brightness;
}

void m5_morsematrix_component::reset_breathing_effect()
{
	breathing_phase_ = 0;
	last_breathing_update_ = 0;
}

void m5_morsematrix_component::draw_morse_clear_animation(uint8_t column, uint8_t max_brightness, uint8_t fade_count)
{
	if (column >= kMatrixWidth) {
		return;
	}

	if (max_brightness >= kBrightnessLevelCount) {
		max_brightness = static_cast<uint8_t>(kBrightnessLevelCount - 1);
	}

	// 渐隐动画：从最大亮度逐渐降到0，重复 fade_count 次
	constexpr char kPattern[] = "--------";  // 8个短横线填满列
	constexpr TickType_t kFrameDelay = pdMS_TO_TICKS(40);  // 每帧40ms

	for (uint8_t cycle = 0; cycle < fade_count; ++cycle) {
		// 渐亮：从0到最大亮度
		for (int8_t level = 0; level <= static_cast<int8_t>(max_brightness); ++level) {
			draw_morse_input_in_col(kPattern, column, static_cast<uint8_t>(level), static_cast<uint8_t>(level));
			vTaskDelay(kFrameDelay);
		}

		// 短暂停留
		vTaskDelay(pdMS_TO_TICKS(100));

		// 渐隐：从最大亮度到0
		for (int8_t level = static_cast<int8_t>(max_brightness); level >= 0; --level) {
			draw_morse_input_in_col(kPattern, column, static_cast<uint8_t>(level), static_cast<uint8_t>(level));
			vTaskDelay(kFrameDelay);
		}

		// 循环间隔
		if (cycle < fade_count - 1) {
			vTaskDelay(pdMS_TO_TICKS(80));
		}
	}

	// 最后清空该列
	draw_morse_input_in_col("", column, 0, 0);
}

void m5_morsematrix_component::update_text_carousel(const char* text, uint32_t interval_ms,
													char_animation_t animation,
													const m5_morsematrix_font_t* font,
													uint8_t brightness_level)
{
	if (!text || text[0] == '\0') {
		return;
	}

	if (!font) {
		font = &g_m5_morsematrix_font_normal_8x8;
	}

	if (brightness_level == 0xFF) {
		brightness_level = default_brightness_level_;
	}

	TickType_t now = xTaskGetTickCount();

	// 初始化
	if (last_carousel_update_ == 0) {
		last_carousel_update_ = now;
		carousel_char_index_ = 0;
	}

	// 检查是否需要切换字符
	if (pdTICKS_TO_MS(now - last_carousel_update_) >= interval_ms) {
		size_t text_length = std::strlen(text);
		if (text_length == 0) {
			return;
		}

		// 显示当前字符
		draw_char_with_animation(0, 0, text[carousel_char_index_],
								animation, font, 0, 50, brightness_level);

		// 切换到下一个字符
		carousel_char_index_++;
		if (carousel_char_index_ >= text_length) {
			carousel_char_index_ = 0;
		}

		last_carousel_update_ = now;
	}
}

void m5_morsematrix_component::reset_text_carousel()
{
	carousel_char_index_ = 0;
	last_carousel_update_ = 0;
}
