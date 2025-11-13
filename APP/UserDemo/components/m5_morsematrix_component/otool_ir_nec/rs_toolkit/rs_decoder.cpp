/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "rs_decoder.h"
#include "esp_log.h"

#ifdef RS_MODE_TRUE_CORRECTION
#include "rs_true_codec.h"
#endif

static const char* TAG = "RSDecoder";

// Reed-Solomon decoder for MorseMatrix IR data reception
// Current mode: RS_MODE_NAME

bool RSDecoder::decodeToBytes(const std::vector<uint8_t>& input, std::vector<uint8_t>& output)
{
    if (input.empty()) {
        ESP_LOGE(TAG, "input is empty");
        return false;
    }

#ifdef RS_MODE_TRUE_CORRECTION
    // Use true Reed-Solomon codec
    const auto* profile = RSEncoder::findProfileByCodewordLength(input.size());
    if (profile == nullptr) {
        ESP_LOGE(TAG, "%s invalid codeword size: %zu",
                 RS_MODE_NAME_BASE, input.size());
        return false;
    }
    const char* mode_name = profile->name;

    std::vector<uint8_t> working_codeword(input.begin(), input.end());
    if (profile->usesExtendedParity()) {
        if (working_codeword.size() != profile->codeword_length) {
            ESP_LOGE(TAG, "%s received %zu bytes but expected %u", mode_name,
                     working_codeword.size(), profile->codeword_length);
            return false;
        }

        uint8_t parity_check = 0;
        for (uint8_t byte : working_codeword) {
            parity_check ^= byte;
        }
        if (parity_check != 0) {
            ESP_LOGW(TAG, "%s overall parity mismatch (xor=0x%02X)", mode_name, parity_check);
        }

        working_codeword.resize(profile->codec_n);
    }
    
    ReedSolomonCodec codec(profile->codec_n, profile->codec_k);
    int errors_corrected = 0;
    std::vector<uint8_t> decoded_block;

    if (!codec.decode(working_codeword, decoded_block, &errors_corrected)) {
        ESP_LOGE(TAG, "%s decoding failed (too many errors)", mode_name);
        return false;
    }

    if (decoded_block.size() != static_cast<size_t>(profile->codec_k)) {
        ESP_LOGE(TAG, "%s decoded block size mismatch: %zu (expected %u)",
                 mode_name, decoded_block.size(), profile->codec_k);
        return false;
    }

    if (decoded_block.size() < RS_LENGTH_FIELD_BYTES) {
        ESP_LOGE(TAG, "%s decoded block missing length header", mode_name);
        return false;
    }

    const uint16_t payload_len =
        static_cast<uint16_t>(decoded_block[0] << 8) | static_cast<uint16_t>(decoded_block[1]);

    if (payload_len > profile->maxPayload()) {
        ESP_LOGE(TAG, "%s decoded invalid payload length %u (max %zu)",
                 mode_name, payload_len, profile->maxPayload());
        return false;
    }

    if (payload_len > decoded_block.size() - RS_LENGTH_FIELD_BYTES) {
        ESP_LOGE(TAG, "%s decoded payload length %u exceeds block capacity %zu", mode_name,
                 payload_len, decoded_block.size() - RS_LENGTH_FIELD_BYTES);
        return false;
    }

    output.assign(decoded_block.begin() + RS_LENGTH_FIELD_BYTES,
                  decoded_block.begin() + RS_LENGTH_FIELD_BYTES + payload_len);
    
    if (errors_corrected > 0) {
        ESP_LOGW(TAG, "%s decoded with %d errors corrected", mode_name, errors_corrected);
    } else {
        ESP_LOGI(TAG, "%s decoded successfully (no errors), payload %u bytes", mode_name,
                 payload_len);
    }
    
    return true;
    
#else  // RS_MODE_PARITY_CHECK
    // Legacy parity-based mode
    if (input.size() < 2) {
        ESP_LOGE(TAG, "%s encoded payload too short (%zu bytes)", RS_MODE_NAME, input.size());
        return false;
    }

    output.clear();
    output.reserve(input.size());

    size_t index = 0;
    size_t chunk = 0;
    size_t error_chunks = 0;

    while (index < input.size()) {
        size_t remaining = input.size() - index;
        if (remaining <= 1) {
            ESP_LOGW(TAG, "%s missing parity byte for chunk %zu, skipping remaining %zu bytes", 
                     RS_MODE_NAME, chunk, remaining);
            break;  // Skip incomplete chunk at end, don't fail entire decode
        }

        size_t data_len = (remaining > (RS_DATA_CHUNK_SIZE + 1)) ? RS_DATA_CHUNK_SIZE : (remaining - 1);
        const uint8_t* data_ptr = input.data() + index;
        uint8_t parity = input[index + data_len];

        uint8_t expected = RSEncoder::calculateChecksum(data_ptr, data_len);
        if (expected != parity) {
            ESP_LOGW(TAG, "%s parity mismatch in chunk %zu (expected 0x%02X, got 0x%02X) - SKIPPING CHUNK", 
                    RS_MODE_NAME, chunk, expected, parity);
            error_chunks++;
            // Skip this chunk but continue decoding
            index += data_len + 1;
            chunk++;
            continue;
        }

        // Valid chunk - add to output
        output.insert(output.end(), data_ptr, data_ptr + data_len);
        index += data_len + 1; // advance past data bytes and parity byte
        chunk++;
    }

    if (error_chunks > 0) {
        ESP_LOGW(TAG, "%s decoded with %zu error chunks skipped, recovered %zu bytes from %zu encoded bytes", 
                RS_MODE_NAME, error_chunks, output.size(), input.size());
        // Return false because not all chunks were valid
        return false;
    } else {
        ESP_LOGI(TAG, "%s decoded %zu bytes from %zu encoded bytes (all chunks valid)", 
                RS_MODE_NAME, output.size(), input.size());
        // Return true only if all chunks passed validation
        return true;
    }
#endif  // RS_MODE_TRUE_CORRECTION / RS_MODE_PARITY_CHECK
}

bool RSDecoder::decode(const std::vector<uint8_t>& input, std::string& output)
{
    std::vector<uint8_t> buffer;
    if (!decodeToBytes(input, buffer)) {
        return false;
    }

#ifdef RS_MODE_TRUE_CORRECTION
    const size_t max_payload = RSEncoder::maxPayloadAcrossProfiles();
    if (buffer.size() > max_payload) {
        ESP_LOGE(TAG, "%s decoded data too large (%zu bytes, max %zu)",
                 RS_MODE_NAME_BASE, buffer.size(), max_payload);
        return false;
    }
#else
    // Validate decoded size doesn't exceed maximum
    if (buffer.size() > RS_MAX_DATA_SIZE) {
        ESP_LOGE(TAG, "%s decoded data too large (%zu bytes, max %d)",
                RS_MODE_NAME, buffer.size(), RS_MAX_DATA_SIZE);
        return false;
    }
#endif

    output.assign(buffer.begin(), buffer.end());
    return true;
}
