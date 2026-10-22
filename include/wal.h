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
        void ensure_writable() const;
        // Bytes cut from the end of the file by recover() because the final
        // record was torn or incomplete. Zero when the log was clean.
        uint64_t truncated_tail_bytes() const { return truncated_tail_bytes_; }
    private:
        int fd_;
        uint64_t truncated_tail_bytes_ = 0;
        bool failed_ = false; // Access is serialized by the dispatcher mutation mutex.
};
