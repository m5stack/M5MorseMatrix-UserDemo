/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __M5_MORSEMATRIX_IR_H__
#define __M5_MORSEMATRIX_IR_H__

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ir_nec_tools.h"

class m5_morsematrix_component;

class m5_morsematrix_ir {
public:
	m5_morsematrix_ir();

	void set_owner(m5_morsematrix_component* owner);

	void setup_led_show_task();
	void setup_morse_task();

	bool read_morse_packet(uint8_t* sequence, uint8_t* data);
	size_t get_morse_packet_count() const;

	m5_morsematrix_component* owner() const { return owner_; }

private:
	static void ir_nec_receive_forwarder(uint16_t address, uint16_t command, bool is_repeat);
	static void ir_nec_morse_receive_forwarder(uint16_t address, uint16_t command, bool is_repeat);

	static void ir_display_task(void* pv);
	static void ir_morse_process_task(void* pv);

	void run_ir_display_task();
	void run_ir_morse_process_task();

	void initialize_morse_store();
	void handle_led_show_command(uint16_t address, uint16_t command, bool is_repeat);
	void handle_morse_receive(uint16_t address, uint16_t command, bool is_repeat);

	static m5_morsematrix_ir* instance_;

	m5_morsematrix_component* owner_ = nullptr;
	ir_nec_tools ir_driver_;
	QueueHandle_t ir_cmd_queue_ = nullptr;
	QueueHandle_t ir_morse_queue_ = nullptr;
	TaskHandle_t ir_display_task_handle_ = nullptr;
	TaskHandle_t ir_morse_task_handle_ = nullptr;
};

#endif // __M5_MORSEMATRIX_IR_H__
