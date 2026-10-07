#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "command.h"
#include "persistence_format.h"

inline constexpr uint8_t OPCODE_SET = 0;
inline constexpr uint8_t OPCODE_DEL = 1;
inline constexpr uint8_t OPCODE_CLEAR = 2;

inline constexpr uint32_t WAL_HEADER_SIZE = PERSISTENCE_RECORD_HEADER_SIZE;
inline constexpr uint32_t WAL_MIN_PAYLOAD_SIZE = PERSISTENCE_MIN_PAYLOAD_SIZE;
inline constexpr uint32_t WAL_MAX_PAYLOAD_SIZE = PERSISTENCE_MAX_PAYLOAD_SIZE;

class WAL {
    public:
        WAL();
        WAL(const std::string& path);
        ~WAL();

        void write_ahead(uint8_t opcode, const std::string& key = "", const std::string& value = "");
        std::vector<Command> recover();
        void reset();
    private:
        int fd_;
};
