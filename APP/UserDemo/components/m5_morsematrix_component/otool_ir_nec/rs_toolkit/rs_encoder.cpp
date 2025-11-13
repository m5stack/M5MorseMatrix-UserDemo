/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "rs_encoder.h"
#include "esp_log.h"
#include <algorithm>
#include <array>

#ifdef RS_MODE_TRUE_CORRECTION
#include "rs_true_codec.h"

namespace {

constexpr std::array<RSEncoder::Profile, 4> kProfiles = {{
    {64, 48, 16, 8,  "RS(64,48) True Correction", 64, 48},
    {128, 96, 32, 16, "RS(128,96) True Correction", 128, 96},
    {192, 144, 48, 24, "RS(192,144) True Correction", 192, 144},
    {256, 191, 65, 32, "RS(256,191) True Correction", 255, 191},
}};

}  // namespace

static const char* TAG = "RSEncoder";

// Reed-Solomon encoding implementation for MorseMatrix
// Current mode: RS_MODE_NAME

const RSEncoder::Profile& RSEncoder::selectProfile(size_t payload_length)
{
    size_t required = payload_length + RS_LENGTH_FIELD_BYTES;
    for (const auto& profile : kProfiles) {
        if (required <= profile.data_bytes) {
            return profile;
        }
    }
    return kProfiles.back();
}

const RSEncoder::Profile* RSEncoder::findProfileByCodewordLength(size_t codeword_length)
{
    for (const auto& profile : kProfiles) {
        if (profile.codeword_length == codeword_length) {
            return &profile;
        }
    }
    return nullptr;
}

size_t RSEncoder::maxPayloadAcrossProfiles()
{
    size_t max_payload = 0;
    for (const auto& profile : kProfiles) {
        max_payload = std::max(max_payload, profile.maxPayload());
    }
    return max_payload;
}
#endif

bool RSEncoder::encode(const std::string& input, std::vector<uint8_t>& output)
{
    if (input.empty()) {
        ESP_LOGE(TAG, "input is empty");
        return false;
    }

#ifdef RS_MODE_TRUE_CORRECTION
    const size_t max_payload = maxPayloadAcrossProfiles();
    if (input.length() > max_payload) {
        ESP_LOGE(TAG, "input too large (%zu bytes), max is %zu bytes for %s", 
                input.length(), max_payload, RS_MODE_NAME_BASE);
        return false;
    }

    // Use true Reed-Solomon codec
    const size_t payload_len = input.size();
    const auto& profile = selectProfile(payload_len);

    std::vector<uint8_t> data(profile.codec_k, 0);
    data[0] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    data[1] = static_cast<uint8_t>(payload_len & 0xFF);
    std::copy(input.begin(), input.end(), data.begin() + RS_LENGTH_FIELD_BYTES);
    ReedSolomonCodec codec(profile.codec_n, profile.codec_k);
    
    if (!codec.encode(data, output)) {
        ESP_LOGE(TAG, "RS encoding failed");
        return false;
    }

    if (profile.usesExtendedParity()) {
        uint8_t overall = 0;
        for (uint8_t byte : output) {
            overall ^= byte;
        }
        output.push_back(overall);
    }

    if (output.size() != profile.codeword_length) {
        ESP_LOGE(TAG, "%s produced %zu bytes, expected %u", profile.name,
                 output.size(), profile.codeword_length);
        return false;
    }
    
    ESP_LOGI(TAG, "%s encoded %zu bytes to %zu bytes (can correct up to %u errors)", 
            profile.name, input.length(), output.size(), profile.maxErrors);
    return true;
    
#else  // RS_MODE_PARITY_CHECK
    if (input.length() > RS_MAX_DATA_SIZE) {
        ESP_LOGE(TAG, "input too large (%zu bytes), max is %d bytes for %s",
                input.length(), RS_MAX_DATA_SIZE, RS_MODE_NAME);
        return false;
    }

    // Legacy parity-based mode
    output.clear();
    
    // Convert string to bytes
    for (char c : input) {
        output.push_back(static_cast<uint8_t>(c));
    }
    
    // Add parity bytes for error correction
    std::vector<uint8_t> encoded;
    addParity(output, encoded);
    
    output = encoded;
    
    ESP_LOGI(TAG, "%s encoded %zu bytes to %zu bytes", RS_MODE_NAME, input.length(), output.size());
    return true;
#endif
}

#ifdef RS_MODE_PARITY_CHECK
// Legacy parity check functions (only used in parity mode)

uint8_t RSEncoder::calculateChecksum(const uint8_t* data, size_t length)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum ^= data[i];
        // Simple polynomial mixing
        if (checksum & 0x80) {
            checksum = (checksum << 1) ^ 0x07;
        } else {
            checksum <<= 1;
        }
    }
    return checksum;
}

void RSEncoder::addParity(const std::vector<uint8_t>& data, std::vector<uint8_t>& output)
{
    output.clear();
    
    // Reserve space for data + parity (every RS_DATA_CHUNK_SIZE data bytes, add 1 parity byte)
    size_t parity_count = (data.size() + RS_DATA_CHUNK_SIZE - 1) / RS_DATA_CHUNK_SIZE;
    output.reserve(data.size() + parity_count);
    
    // Add data with interleaved parity
    for (size_t i = 0; i < data.size(); i += RS_DATA_CHUNK_SIZE) {
        // Add up to RS_DATA_CHUNK_SIZE data bytes
        size_t chunk_size = std::min(size_t(RS_DATA_CHUNK_SIZE), data.size() - i);
        for (size_t j = 0; j < chunk_size; j++) {
            output.push_back(data[i + j]);
        }
        
        // Calculate and add parity byte for this chunk
        uint8_t parity = calculateChecksum(data.data() + i, chunk_size);
        output.push_back(parity);
    }
    
    ESP_LOGD(TAG, "%s added %zu parity bytes to %zu data bytes", 
            RS_MODE_NAME, parity_count, data.size());
}
#endif  // RS_MODE_PARITY_CHECK
