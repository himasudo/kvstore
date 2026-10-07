#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

inline constexpr uint32_t PERSISTENCE_FORMAT_VERSION = 1;
inline constexpr size_t PERSISTENCE_FILE_HEADER_SIZE = 8;
inline constexpr size_t PERSISTENCE_RECORD_HEADER_SIZE = 8;
inline constexpr uint32_t PERSISTENCE_MIN_PAYLOAD_SIZE = 9;
inline constexpr uint32_t PERSISTENCE_MAX_PAYLOAD_SIZE = 2 * 1024 * 1024;

struct PersistenceRecordHeader {
    uint32_t payload_size;
    uint32_t checksum;
};

struct PersistenceRecord {
    uint8_t opcode;
    std::string key;
    std::string value;
};

uint32_t crc32c(std::span<const uint8_t> bytes);

std::array<uint8_t, PERSISTENCE_FILE_HEADER_SIZE> make_file_header(
    const std::array<uint8_t, 4>& magic);
void validate_file_header(std::span<const uint8_t> header,
                          const std::array<uint8_t, 4>& expected_magic);

PersistenceRecordHeader decode_record_header(std::span<const uint8_t> header);
std::vector<uint8_t> encode_record(uint8_t opcode,
                                   const std::string& key,
                                   const std::string& value);
PersistenceRecord decode_record_payload(std::span<const uint8_t> payload,
                                        uint32_t expected_checksum);
