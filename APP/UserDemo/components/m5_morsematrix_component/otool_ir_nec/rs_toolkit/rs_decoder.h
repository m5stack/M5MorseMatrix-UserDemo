/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Include encoder header to get RS mode configuration
#include "rs_encoder.h"

/**
 * @brief Decoder counterpart for the MorseMatrix encoder.
 *
 * Performs full Reed-Solomon error correction in true RS mode and parity validation in
 * legacy parity mode.
 *
 * Note: Encoder and decoder must use the same mode.
 */
class RSDecoder {
public:
    /**
     * @brief Decode the parity-augmented payload back into a byte vector.
     *
     * @param input Encoded bytes (data chunks interleaved with parity).
     * @param output Vector populated with decoded payload bytes (valid chunks only).
     * @return true ONLY if ALL chunks passed parity validation, false if any errors detected.
     * 
     * Note: Automatically uses RS_DATA_CHUNK_SIZE from encoder configuration.
     * Error-tolerant decoding: chunks with parity errors are skipped and logged,
     * but decoding continues for subsequent valid chunks. However, the function
     * returns false if ANY chunk fails validation, even though output may contain
     * partial recovered data. This allows the caller to decide whether to use
     * the partial data or reject it entirely.
     */
    static bool decodeToBytes(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

    /**
     * @brief Decode the payload and return it as a std::string.
     *
     * @param input Encoded bytes (data chunks interleaved with parity).
     * @param output String populated with decoded payload (valid chunks only).
     * @return true ONLY if ALL chunks passed validation, false if any errors detected.
     * 
     * Note: Automatically uses RS_DATA_CHUNK_SIZE from encoder configuration.
     * Error-tolerant: skips corrupted chunks but returns false if any chunk fails.
     * Output may contain partial data even when function returns false.
     */
    static bool decode(const std::vector<uint8_t>& input, std::string& output);
    
    /**
     * @brief Get the current RS mode name for logging/debugging.
     * 
     * @return const char* RS mode name (e.g., "RS(127,111)")
     */
    static const char* getModeName() {
#ifdef RS_MODE_TRUE_CORRECTION
        return RS_MODE_NAME_BASE;
#else
        return RS_MODE_NAME;
#endif
    }
    
    /**
     * @brief Get the maximum data size for current RS mode.
     * 
     * @return size_t Maximum data bytes that can be decoded
     */
    static size_t getMaxDataSize() {
#ifdef RS_MODE_TRUE_CORRECTION
        return RSEncoder::maxPayloadAcrossProfiles();
#else
        return RS_MAX_DATA_SIZE;
#endif
    }
};
