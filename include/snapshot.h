#pragma once

#include <cstdint>
#include <string>
#include "kvstore.h"
#include "persistence_format.h"

inline constexpr uint8_t SNAPSHOT_OPCODE_SET = 0;
inline constexpr uint32_t SNAPSHOT_HEADER_SIZE = PERSISTENCE_RECORD_HEADER_SIZE;
inline constexpr uint32_t SNAPSHOT_MIN_PAYLOAD_SIZE = PERSISTENCE_MIN_PAYLOAD_SIZE;
inline constexpr uint32_t SNAPSHOT_MAX_PAYLOAD_SIZE = PERSISTENCE_MAX_PAYLOAD_SIZE;

class Snapshot {
    public:
        void write(const KVStore& store, const std::string& path);
        void recover(KVStore& store, const std::string& path);
};
