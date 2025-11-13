/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "m5_morsematrix_ir.h"

#include "m5_morsematrix_component.h"
#include "m5_morsematrix_pinmap.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "rs_decoder.h"

static const char* IR_TAG = "m5_morsematrix_ir";

namespace {

struct IRCommand {
	uint8_t start_index;
	uint8_t end_index;
	uint32_t duration_s;
};

struct IRMorsePacket {
	uint8_t sequence;
	uint8_t data;
};

constexpr size_t kIRMorsePacketCapacity = 255;
constexpr size_t kIRMorseQueueSize = 64;

struct IRMorsePacketSlot {
	uint8_t sequence = 0;
	uint8_t data = 0;
	bool has_valid_packet = false;
};

struct IRMorseReceptionState {
	uint32_t reception_index = 0;
	uint8_t highest_sequence_seen = 0;
	uint8_t tail_packet_length = 0;
	bool tail_packet_length_known = false;
	uint16_t valid_packet_count = 0;
	IRMorsePacketSlot packets[kIRMorsePacketCapacity] = {};
	uint8_t received_bytes[kIRMorsePacketCapacity] = {};
	char decoded_text[kIRMorsePacketCapacity + 1] = {};
};

struct IRMorseSnapshot {
	uint32_t reception_index = 0;
	uint16_t valid_packet_count = 0;
	uint8_t highest_sequence_seen = 0;
	bool tail_packet_length_known = false;
	uint8_t tail_packet_length = 0;
	TickType_t last_packet_tick = 0;
	uint8_t data[kIRMorsePacketCapacity] = {};
	uint8_t valid_flags[kIRMorsePacketCapacity] = {};
};

portMUX_TYPE g_ir_morse_lock = portMUX_INITIALIZER_UNLOCKED;
IRMorseReceptionState g_ir_morse_state;
uint32_t g_ir_morse_reception_counter = 0;
bool g_ir_morse_session_active = false;
TickType_t g_ir_morse_last_packet_time = 0;
constexpr TickType_t kIRMorseSessionTimeoutTicks = pdMS_TO_TICKS(1000);

void clear_morse_packets_locked() {
	for (size_t i = 0; i < kIRMorsePacketCapacity; ++i) {
		g_ir_morse_state.packets[i].sequence = static_cast<uint8_t>(i);
		g_ir_morse_state.packets[i].data = 0;
		g_ir_morse_state.packets[i].has_valid_packet = false;
	}
	std::memset(g_ir_morse_state.received_bytes, 0, sizeof(g_ir_morse_state.received_bytes));
	std::memset(g_ir_morse_state.decoded_text, 0, sizeof(g_ir_morse_state.decoded_text));
}

void reset_morse_session_locked(bool start_new_session) {
	g_ir_morse_state.highest_sequence_seen = 0;
	g_ir_morse_state.tail_packet_length = 0;
	g_ir_morse_state.tail_packet_length_known = false;
	g_ir_morse_state.valid_packet_count = 0;
	if (start_new_session) {
		++g_ir_morse_reception_counter;
		g_ir_morse_state.reception_index = g_ir_morse_reception_counter;
		g_ir_morse_session_active = true;
	} else {
		g_ir_morse_state.reception_index = 0;
		g_ir_morse_session_active = false;
	}
	clear_morse_packets_locked();
}

void start_new_morse_session_locked() {
	reset_morse_session_locked(true);
}

bool validate_nec_word(uint16_t word) {
	uint8_t low = static_cast<uint8_t>(word & 0xFFU);
	uint8_t high = static_cast<uint8_t>((word >> 8) & 0xFFU);
	return static_cast<uint8_t>(low ^ high) == 0xFFU;
}

void store_ir_morse_packet_from_task(uint8_t sequence, uint8_t data_byte) {
	bool out_of_range = false;

	portENTER_CRITICAL(&g_ir_morse_lock);

	if (sequence >= kIRMorsePacketCapacity) {
		out_of_range = true;
	} else {
		if (!g_ir_morse_session_active || sequence == 0) {
			start_new_morse_session_locked();
		}

		IRMorsePacketSlot& slot = g_ir_morse_state.packets[sequence];
		bool was_valid = slot.has_valid_packet;

		slot.sequence = sequence;
		slot.data = data_byte;
		slot.has_valid_packet = true;

		g_ir_morse_state.received_bytes[sequence] = data_byte;
		if (!was_valid && g_ir_morse_state.valid_packet_count < kIRMorsePacketCapacity) {
			++g_ir_morse_state.valid_packet_count;
		}

		if (sequence > g_ir_morse_state.highest_sequence_seen) {
			g_ir_morse_state.highest_sequence_seen = sequence;
		}

		g_ir_morse_last_packet_time = xTaskGetTickCount();
	}

	portEXIT_CRITICAL(&g_ir_morse_lock);

	if (out_of_range) {
		ESP_LOGW(IR_TAG, "IR Morse sequence %u out of range", static_cast<unsigned>(sequence));
	}
}

bool rs_decode_to_bytes_tolerant(const std::vector<uint8_t>& input,
								 std::vector<uint8_t>& output,
								 size_t* out_total_chunks = nullptr,
								 size_t* out_failed_chunks = nullptr) {
	if (input.empty()) {
		ESP_LOGE(IR_TAG, "RS decode [Tolerant]: input is empty");
		return false;
	}

#ifdef RS_MODE_TRUE_CORRECTION
	bool decode_success = RSDecoder::decodeToBytes(input, output);

	if (out_total_chunks) *out_total_chunks = 1;
	if (out_failed_chunks) *out_failed_chunks = decode_success ? 0 : 1;

	if (!decode_success) {
		ESP_LOGW(IR_TAG, "RS decode [%s] [Tolerant]: decoding failed (too many errors)",
				 RSDecoder::getModeName());
	}

	return decode_success;

#else
	if (input.size() < 2) {
		ESP_LOGE(IR_TAG, "RS decode [Tolerant]: encoded payload too short (%u bytes)",
				 static_cast<unsigned>(input.size()));
		return false;
	}

	output.clear();
	output.reserve(input.size());

	size_t index = 0;
	size_t chunk = 0;
	size_t failed_chunks = 0;

	while (index < input.size()) {
		size_t remaining = input.size() - index;
		if (remaining <= 1) {
			ESP_LOGW(IR_TAG, "RS decode [Tolerant]: missing parity byte for chunk %u, skipping",
					 static_cast<unsigned>(chunk));
			break;
		}

		size_t data_len = (remaining > (RS_DATA_CHUNK_SIZE + 1)) ? RS_DATA_CHUNK_SIZE : (remaining - 1);
		const uint8_t* data_ptr = input.data() + index;
		uint8_t parity = input[index + data_len];

		uint8_t expected = RSEncoder::calculateChecksum(data_ptr, data_len);
		bool chunk_valid = (expected == parity);

		if (!chunk_valid) {
			failed_chunks++;
			ESP_LOGW(IR_TAG,
					 "RS decode [%s] [Tolerant]: parity mismatch in chunk %u (expected 0x%02X, got 0x%02X) - keeping data anyway",
					 RSDecoder::getModeName(),
					 static_cast<unsigned>(chunk),
					 expected,
					 parity);
		}

		for (size_t i = 0; i < data_len; ++i) {
			output.push_back(data_ptr[i]);
		}

		index += data_len + 1;
		chunk++;
	}

	if (out_total_chunks) *out_total_chunks = chunk;
	if (out_failed_chunks) *out_failed_chunks = failed_chunks;

	if (output.empty()) {
		ESP_LOGE(IR_TAG, "RS decode [Tolerant]: no data decoded");
		return false;
	}

	if (failed_chunks > 0) {
		ESP_LOGW(IR_TAG,
				 "RS decode [%s] [Tolerant]: decoded %u bytes from %u encoded bytes (%u/%u chunks had errors, %.1f%% error rate)",
				 RSDecoder::getModeName(),
				 static_cast<unsigned>(output.size()),
				 static_cast<unsigned>(input.size()),
				 static_cast<unsigned>(failed_chunks),
				 static_cast<unsigned>(chunk),
				 (chunk > 0) ? (100.0f * failed_chunks / chunk) : 0.0f);
	} else {
		ESP_LOGI(IR_TAG,
				 "RS decode [%s] [Tolerant]: decoded %u bytes from %u encoded bytes (all %u chunks valid)",
				 RSDecoder::getModeName(),
				 static_cast<unsigned>(output.size()),
				 static_cast<unsigned>(input.size()),
				 static_cast<unsigned>(chunk));
	}

	return true;
#endif
}

bool capture_morse_snapshot(IRMorseSnapshot* snapshot, bool reset_after) {
	if (!snapshot) {
		return false;
	}

	bool has_packets = false;

	portENTER_CRITICAL(&g_ir_morse_lock);

	if (g_ir_morse_session_active && g_ir_morse_state.valid_packet_count > 0) {
		has_packets = true;

		snapshot->reception_index = g_ir_morse_state.reception_index;
		snapshot->valid_packet_count = g_ir_morse_state.valid_packet_count;
		snapshot->highest_sequence_seen = g_ir_morse_state.highest_sequence_seen;
		snapshot->tail_packet_length_known = g_ir_morse_state.tail_packet_length_known;
		snapshot->tail_packet_length = g_ir_morse_state.tail_packet_length;
		snapshot->last_packet_tick = g_ir_morse_last_packet_time;

		std::memset(snapshot->valid_flags, 0, sizeof(snapshot->valid_flags));
		std::memcpy(snapshot->data,
					g_ir_morse_state.received_bytes,
					sizeof(snapshot->data));

		for (size_t i = 0; i < kIRMorsePacketCapacity; ++i) {
			snapshot->valid_flags[i] = g_ir_morse_state.packets[i].has_valid_packet ? 1 : 0;
		}

		if (reset_after) {
			reset_morse_session_locked(false);
			g_ir_morse_last_packet_time = 0;
		}
	}

	if (!has_packets && reset_after) {
		reset_morse_session_locked(false);
		g_ir_morse_last_packet_time = 0;
	}

	portEXIT_CRITICAL(&g_ir_morse_lock);

	return has_packets;
}

void process_morse_snapshot(const IRMorseSnapshot& snapshot, m5_morsematrix_ir* module) {
	if (!module) {
		return;
	}

	if (snapshot.valid_packet_count == 0) {
		ESP_LOGW(IR_TAG, "Morse snapshot has no valid packets");
		return;
	}

	std::vector<uint8_t> encoded_data;
	encoded_data.reserve(snapshot.highest_sequence_seen + 1);

	uint16_t missing_count = 0;
	for (uint16_t seq = 0; seq <= snapshot.highest_sequence_seen; ++seq) {
		if (snapshot.valid_flags[seq]) {
			encoded_data.push_back(snapshot.data[seq]);
		} else {
			encoded_data.push_back(0x00);
			missing_count++;
			ESP_LOGW(IR_TAG, "Missing packet at seq=%u, filled with 0x00 placeholder", seq);
		}
	}

	if (encoded_data.empty()) {
		ESP_LOGW(IR_TAG, "No valid packets to decode (after filtering)");
		return;
	}

	if (missing_count > 0) {
		ESP_LOGW(IR_TAG, "Total %u missing packets out of %u (%.1f%% packet loss)",
				 missing_count,
				 snapshot.highest_sequence_seen + 1,
				 100.0f * missing_count / (snapshot.highest_sequence_seen + 1));
	}

	std::vector<uint8_t> decoded_bytes;
	size_t total_chunks = 0;
	size_t failed_chunks = 0;
	bool decode_ok = rs_decode_to_bytes_tolerant(encoded_data, decoded_bytes, &total_chunks, &failed_chunks);

	if (!decode_ok) {
		ESP_LOGE(IR_TAG,
				 "RS Decode FAILED: reception #%lu, %u packets (unable to decode any data)",
				 static_cast<unsigned long>(snapshot.reception_index),
				 static_cast<unsigned>(encoded_data.size()));
		return;
	}

	auto is_printable_char = [](uint8_t c) {
		return (c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t';
	};

	bool has_non_printable = !decoded_bytes.empty() &&
		!std::all_of(decoded_bytes.begin(), decoded_bytes.end(), is_printable_char);

	float error_rate = (total_chunks > 0)
						   ? (static_cast<float>(failed_chunks) / static_cast<float>(total_chunks))
						   : 0.0f;
	bool parse_success = (failed_chunks == 0) && !has_non_printable;

	std::string decoded_text(decoded_bytes.begin(), decoded_bytes.end());

	if (failed_chunks > 0) {
		ESP_LOGW(IR_TAG,
				 "RS Decode PARTIAL SUCCESS: reception #%lu, %u packets -> %u bytes decoded (with %u/%u chunk errors)",
				 static_cast<unsigned long>(snapshot.reception_index),
				 static_cast<unsigned>(encoded_data.size()),
				 static_cast<unsigned>(decoded_bytes.size()),
				 static_cast<unsigned>(failed_chunks),
				 static_cast<unsigned>(total_chunks));
	} else {
		ESP_LOGI(IR_TAG,
				 "RS Decode SUCCESS: reception #%lu, %u packets -> %u bytes decoded (all chunks valid)",
				 static_cast<unsigned long>(snapshot.reception_index),
				 static_cast<unsigned>(encoded_data.size()),
				 static_cast<unsigned>(decoded_bytes.size()));
	}

	ESP_LOGI(IR_TAG,
			 "Decoded text: %s (error rate: %.1f%%, printable=%s)",
			 decoded_text.c_str(),
			 error_rate * 100.0f,
			 parse_success ? "yes" : "no");

	portENTER_CRITICAL(&g_ir_morse_lock);
	std::memset(g_ir_morse_state.decoded_text, 0, sizeof(g_ir_morse_state.decoded_text));
	size_t copy_len = decoded_text.size();
	if (copy_len >= sizeof(g_ir_morse_state.decoded_text)) {
		copy_len = sizeof(g_ir_morse_state.decoded_text) - 1;
	}
	std::memcpy(g_ir_morse_state.decoded_text, decoded_text.data(), copy_len);
	g_ir_morse_state.decoded_text[copy_len] = '\0';
	g_ir_morse_state.tail_packet_length = static_cast<uint8_t>(encoded_data.size());
	g_ir_morse_state.tail_packet_length_known = true;
	portEXIT_CRITICAL(&g_ir_morse_lock);

	if (auto* component = module->owner()) {
		if (auto callback = component->ir_decode_success_callback()) {
			callback(snapshot.reception_index,
					 decoded_text.c_str(),
					 snapshot.valid_packet_count,
					 error_rate,
					 parse_success);
		}
	}
}

} // namespace

m5_morsematrix_ir* m5_morsematrix_ir::instance_ = nullptr;

m5_morsematrix_ir::m5_morsematrix_ir() {
	instance_ = this;
}

void m5_morsematrix_ir::set_owner(m5_morsematrix_component* owner) {
	owner_ = owner;
	instance_ = this;
}

void m5_morsematrix_ir::setup_led_show_task() {
	if (!owner_) {
		ESP_LOGW(IR_TAG, "Cannot setup IR LED show task without owner");
		return;
	}

	esp_err_t err = ir_driver_.init(GPIO_NUM_NC, MORSEMATRIX_PIN_IR_RX, GPIO_NUM_NC);
	if (err != ESP_OK) {
		ESP_LOGW(IR_TAG, "IR driver init failed: %s", esp_err_to_name(err));
	}

	if (!ir_cmd_queue_) {
		ir_cmd_queue_ = xQueueCreate(8, sizeof(IRCommand));
		if (!ir_cmd_queue_) {
			ESP_LOGW(IR_TAG, "Failed to create IR command queue");
		} else {
			BaseType_t res = xTaskCreate(&m5_morsematrix_ir::ir_display_task,
										 "m5_ir_disp",
										 4096,
										 this,
										 tskIDLE_PRIORITY + 1,
										 &ir_display_task_handle_);
			if (res != pdPASS) {
				ESP_LOGW(IR_TAG, "Failed to create IR display task");
				ir_display_task_handle_ = nullptr;
			}
		}
	}

	ir_driver_.set_receive_callback(&m5_morsematrix_ir::ir_nec_receive_forwarder);
	ir_driver_.start_receive();
}

void m5_morsematrix_ir::setup_morse_task() {
	if (!owner_) {
		ESP_LOGW(IR_TAG, "Cannot setup IR Morse task without owner");
		return;
	}

	esp_err_t err = ir_driver_.init(GPIO_NUM_NC, MORSEMATRIX_PIN_IR_RX, GPIO_NUM_NC);
	if (err != ESP_OK) {
		ESP_LOGW(IR_TAG, "IR driver init failed: %s", esp_err_to_name(err));
	}

	initialize_morse_store();

	if (!ir_morse_queue_) {
		ir_morse_queue_ = xQueueCreate(kIRMorseQueueSize, sizeof(IRMorsePacket));
		if (!ir_morse_queue_) {
			ESP_LOGW(IR_TAG, "Failed to create IR Morse queue");
			return;
		}

		BaseType_t res = xTaskCreate(&m5_morsematrix_ir::ir_morse_process_task,
									 "m5_ir_morse",
									 4096,
									 this,
									 tskIDLE_PRIORITY + 1,
									 &ir_morse_task_handle_);
		if (res != pdPASS) {
			ESP_LOGW(IR_TAG, "Failed to create IR Morse processing task");
			ir_morse_task_handle_ = nullptr;
			return;
		}

		ESP_LOGI(IR_TAG, "IR Morse queue created (size=%zu), processing task started", kIRMorseQueueSize);
	}

	ir_driver_.set_receive_callback(&m5_morsematrix_ir::ir_nec_morse_receive_forwarder);
	ir_driver_.start_receive();

	ESP_LOGI(IR_TAG, "IR Morse receiver started");
}

bool m5_morsematrix_ir::read_morse_packet(uint8_t* sequence, uint8_t* data) {
	if (!ir_morse_queue_) {
		return false;
	}

	IRMorsePacket packet;
	if (xQueueReceive(ir_morse_queue_, &packet, 0) == pdTRUE) {
		if (sequence) *sequence = packet.sequence;
		if (data) *data = packet.data;
		return true;
	}

	return false;
}

size_t m5_morsematrix_ir::get_morse_packet_count() const {
	if (!ir_morse_queue_) {
		return 0;
	}
	return static_cast<size_t>(uxQueueMessagesWaiting(ir_morse_queue_));
}

void m5_morsematrix_ir::initialize_morse_store() {
	portENTER_CRITICAL(&g_ir_morse_lock);
	reset_morse_session_locked(false);
	g_ir_morse_last_packet_time = 0;
	portEXIT_CRITICAL(&g_ir_morse_lock);

	ESP_LOGI(IR_TAG, "RS Decoder initialized: Mode=%s, Max data size=%zu bytes",
			 RSDecoder::getModeName(),
			 RSDecoder::getMaxDataSize());
}

void m5_morsematrix_ir::run_ir_display_task() {
	IRCommand cmd;
	while (true) {
		if (ir_cmd_queue_ && xQueueReceive(ir_cmd_queue_, &cmd, portMAX_DELAY) == pdTRUE) {
			if (owner_) {
				uint32_t duration_ms = cmd.duration_s * 1000u;
				owner_->light_leds_range(cmd.start_index, cmd.end_index, duration_ms);
			}
		}
	}
}

void m5_morsematrix_ir::run_ir_morse_process_task() {
	if (!ir_morse_queue_) {
		vTaskDelete(nullptr);
		return;
	}

	IRMorsePacket packet;
	TickType_t last_check_time = xTaskGetTickCount();
	constexpr TickType_t kCheckInterval = pdMS_TO_TICKS(100);

	while (true) {
		if (xQueueReceive(ir_morse_queue_, &packet, kCheckInterval) == pdTRUE) {
			store_ir_morse_packet_from_task(packet.sequence, packet.data);
		}

		TickType_t now = xTaskGetTickCount();
		if ((now - last_check_time) >= kCheckInterval) {
			last_check_time = now;

			TickType_t idle_time = 0;
			bool session_active = false;

			portENTER_CRITICAL(&g_ir_morse_lock);
			if (g_ir_morse_session_active && g_ir_morse_last_packet_time > 0) {
				idle_time = now - g_ir_morse_last_packet_time;
				session_active = true;
			}
			portEXIT_CRITICAL(&g_ir_morse_lock);

			bool should_decode = false;
			IRMorseSnapshot snapshot{};

			if (session_active && idle_time >= kIRMorseSessionTimeoutTicks) {
				ESP_LOGI(IR_TAG, "IR Morse session timeout, capturing snapshot for decode...");
				should_decode = capture_morse_snapshot(&snapshot, true);
			}

			if (should_decode) {
				process_morse_snapshot(snapshot, this);
			}
		}
	}
}

void m5_morsematrix_ir::handle_led_show_command(uint16_t address, uint16_t command, bool is_repeat) {
	(void)is_repeat;

	uint8_t timeout = static_cast<uint8_t>(address & 0xFF);
	uint8_t count = static_cast<uint8_t>(command & 0x7F);

	if (count == 0 || !ir_cmd_queue_) {
		return;
	}

	IRCommand cmd{};
	cmd.start_index = 0;
	cmd.end_index = (count > 64) ? 63 : static_cast<uint8_t>(count - 1);
	cmd.duration_s = static_cast<uint32_t>(timeout);

	if (xPortInIsrContext()) {
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		(void)xQueueSendFromISR(ir_cmd_queue_, &cmd, &xHigherPriorityTaskWoken);
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	} else {
		if (xQueueSend(ir_cmd_queue_, &cmd, 0) != pdTRUE) {
			ESP_LOGW(IR_TAG, "IR queue full - dropping command");
		}
	}
}

void m5_morsematrix_ir::handle_morse_receive(uint16_t address, uint16_t command, bool is_repeat) {
	if (is_repeat || !ir_morse_queue_) {
		return;
	}

	if (!validate_nec_word(address) || !validate_nec_word(command)) {
		return;
	}

	uint8_t sequence = static_cast<uint8_t>(address & 0xFFU);
	if (sequence >= kIRMorsePacketCapacity) {
		return;
	}

	IRMorsePacket packet{};
	packet.sequence = sequence;
	packet.data = static_cast<uint8_t>(command & 0xFFU);

	if (xPortInIsrContext()) {
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		(void)xQueueSendFromISR(ir_morse_queue_, &packet, &xHigherPriorityTaskWoken);
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	} else {
		if (xQueueSend(ir_morse_queue_, &packet, 0) != pdTRUE) {
			ESP_LOGW(IR_TAG, "Morse queue full - dropping packet");
		}
	}
}

void m5_morsematrix_ir::ir_nec_receive_forwarder(uint16_t address, uint16_t command, bool is_repeat) {
	if (!instance_) {
		return;
	}
	instance_->handle_led_show_command(address, command, is_repeat);
}

void m5_morsematrix_ir::ir_nec_morse_receive_forwarder(uint16_t address, uint16_t command, bool is_repeat) {
	if (!instance_) {
		return;
	}
	instance_->handle_morse_receive(address, command, is_repeat);
}

void m5_morsematrix_ir::ir_display_task(void* pv) {
	auto* module = static_cast<m5_morsematrix_ir*>(pv);
	if (!module) {
		vTaskDelete(nullptr);
		return;
	}
	module->run_ir_display_task();
}

void m5_morsematrix_ir::ir_morse_process_task(void* pv) {
	auto* module = static_cast<m5_morsematrix_ir*>(pv);
	if (!module) {
		vTaskDelete(nullptr);
		return;
	}
	module->run_ir_morse_process_task();
}
