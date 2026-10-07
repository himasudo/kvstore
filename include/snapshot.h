#pragma once

#include <cstdint>
#include <string>
#include "kvstore.h"

inline constexpr uint8_t SNAPSHOT_OPCODE_SET = 0;
inline constexpr uint32_t SNAPSHOT_HEADER_SIZE = 8;
inline constexpr uint32_t SNAPSHOT_MIN_PAYLOAD_SIZE = 9;
inline constexpr uint32_t SNAPSHOT_MAX_PAYLOAD_SIZE = 2 * 1024 * 1024;

class Snapshot {
    public:
        void write(const KVStore& store, const std::string& path);
        void recover(KVStore& store, const std::string& path);
};
