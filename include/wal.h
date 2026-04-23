#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "command.h"

inline constexpr uint8_t OPCODE_SET = 0;
inline constexpr uint8_t OPCODE_DEL = 1;

class WAL {
    public:
        WAL();
        WAL(const std::string& path);
        ~WAL();

        void write_ahead(uint8_t opcode, const std::string& key, const std::string& value = "");
        std::vector<Command> recover();
        void reset();
    private:
        int fd_;
};
