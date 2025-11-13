/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#ifdef RS_MODE_TRUE_CORRECTION
#include <array>
#endif

// RS Encoding Mode Selection
// Define one of these to select encoding scheme:
// - RS_MODE_TRUE_CORRECTION: True Reed-Solomon with error correction (recommended for 50% loss tolerance)
// - RS_MODE_PARITY_CHECK: Simple parity check (legacy, detection only)

#if !defined(RS_MODE_TRUE_CORRECTION) && !defined(RS_MODE_PARITY_CHECK)
    #define RS_MODE_TRUE_CORRECTION  // Default: use true RS
#endif

#ifdef RS_MODE_TRUE_CORRECTION
    inline constexpr size_t RS_LENGTH_FIELD_BYTES = 2;
    inline constexpr const char* RS_MODE_NAME_BASE = "RS True Correction";

#elif defined(RS_MODE_PARITY_CHECK)
    // Legacy parity-based mode (detection only)
    #define RS_DATA_CHUNK_SIZE 4
    #define RS_MAX_DATA_SIZE 111
    #define RS_PARITY_BYTES 16
    #define RS_MODE_NAME "Parity Check (detection only)"
#endif

/**
 * @brief Reed-Solomon encoder for MorseMatrix data transmission
 * 
 * Provides error correction encoding for transmitting text data
 * over IR to the MorseMatrix light panel.
 * 
 * RS_MODE_TRUE_CORRECTION: Adaptive Reed-Solomon coding with configurable payload buckets
 * RS_MODE_PARITY_CHECK: Simple parity check - can only detect errors, no correction
 */
class RSEncoder {
public:
#ifdef RS_MODE_TRUE_CORRECTION
    struct Profile {
        uint16_t codeword_length;
        uint16_t data_bytes;
        uint16_t parity_bytes;
        uint8_t maxErrors;
        const char* name;
        uint16_t codec_n;
        uint16_t codec_k;

        size_t maxPayload() const { return static_cast<size_t>(data_bytes) - RS_LENGTH_FIELD_BYTES; }
        bool usesExtendedParity() const { return codeword_length > codec_n; }
    };

    static const Profile& selectProfile(size_t payload_length);
    static const Profile* findProfileByCodewordLength(size_t codeword_length);
    static size_t maxPayloadAcrossProfiles();
#endif

    /**
     * @brief Encode a string with error correction
     * 
     * @param input Input string to encode
     * @param output Vector to store encoded bytes
     * @return true if encoding successful, false otherwise
     */
    static bool encode(const std::string& input, std::vector<uint8_t>& output);
    
#ifdef RS_MODE_PARITY_CHECK
    // Legacy parity check functions (only available in parity mode)
    
    /**
     * @brief Calculate simple checksum for data validation
     * 
     * @param data Input data
     * @param length Data length
     * @return Checksum value
     */
    static uint8_t calculateChecksum(const uint8_t* data, size_t length);
    
    /**
     * @brief Add parity bytes for error detection
     * 
     * @param data Input data
     * @param output Output with parity
     */
    static void addParity(const std::vector<uint8_t>& data, std::vector<uint8_t>& output);
#endif
};
