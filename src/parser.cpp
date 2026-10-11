#include "parser.h"

#include <cstdint>
#include <limits>
#include <unordered_map>

static const std::unordered_map<std::string, Command::Type> command_map = {
    {"SET", Command::Type::SET},
    {"GET", Command::Type::GET},
    {"KEYS", Command::Type::KEYS},
    {"SIZE", Command::Type::SIZE},
    {"EXISTS", Command::Type::EXISTS},
    {"DEL", Command::Type::DEL},
    {"CLEAR", Command::Type::CLEAR},
};

using LengthResult = std::variant<uint32_t, ParseIncomplete, ParseError>;

static LengthResult read_length(std::string_view input, size_t& pos,
                                char prefix, uint32_t limit) {
    if (pos == input.size()) return ParseIncomplete{};
    if (input[pos++] != prefix) return ParseError{"Invalid RESP length prefix"};

    uint32_t length = 0;
    bool has_digit = false;
    while (pos < input.size()) {
        const char c = input[pos];
        if (c == '\r') {
            if (pos + 1 == input.size()) return ParseIncomplete{};
            if (input[pos + 1] != '\n' || !has_digit) {
                return ParseError{"Invalid RESP length terminator"};
            }
            pos += 2;
            return length;
        }
        if (c < '0' || c > '9') return ParseError{"RESP length must be decimal"};
        uint32_t digit = static_cast<uint32_t>(c - '0');
        if (digit > limit || length > (limit - digit) / 10) {
            return ParseError{"RESP length exceeds limit"};
        }
        length = length * 10 + digit;
        has_digit = true;
        ++pos;
    }
    return ParseIncomplete{};
}

ParseResult parse_one(std::string_view input) {
    size_t pos = 0;
    LengthResult count_result = read_length(
        input, pos, '*', static_cast<uint32_t>(MAX_ARG_COUNT + 1));
    if (auto incomplete = std::get_if<ParseIncomplete>(&count_result)) return *incomplete;
    if (auto error = std::get_if<ParseError>(&count_result)) return *error;

    uint32_t count = std::get<uint32_t>(count_result);
    if (count == 0) return ParseError{"RESP command must not be empty"};

    Command command{};
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t limit = i == 0 ? static_cast<uint32_t>(MAX_COMMAND_SIZE)
                             : i == 1 ? static_cast<uint32_t>(MAX_KEY_SIZE)
                                      : static_cast<uint32_t>(MAX_VALUE_SIZE);
        LengthResult size_result = read_length(input, pos, '$', limit);
        if (auto incomplete = std::get_if<ParseIncomplete>(&size_result)) return *incomplete;
        if (auto error = std::get_if<ParseError>(&size_result)) return *error;
        const size_t size = std::get<uint32_t>(size_result);
        if (i == 0 && size == 0) return ParseError{"RESP command name is empty"};
        if (input.size() - pos < size + 2) return ParseIncomplete{};
        if (input[pos + size] != '\r' || input[pos + size + 1] != '\n') {
            return ParseError{"Invalid RESP bulk-string terminator"};
        }

        std::string token(input.substr(pos, size));
        pos += size + 2;
        if (i == 0) {
            auto it = command_map.find(token);
            if (it == command_map.end()) return ParseError{"Unknown command"};
            command.type = it->second;
        } else {
            command.args.push_back(std::move(token));
        }
    }

    return ParseComplete{std::move(command), pos};
}

std::expected<Command, std::string> parse(std::string_view input) {
    ParseResult result = parse_one(input);
    if (auto complete = std::get_if<ParseComplete>(&result)) {
        if (complete->consumed != input.size()) {
            return std::unexpected("Error[Invalid Input]: trailing bytes after command");
        }
        return std::move(complete->command);
    }
    if (std::holds_alternative<ParseIncomplete>(result)) {
        return std::unexpected("Error[Invalid Input]: incomplete command");
    }
    return std::unexpected("Error[Invalid Input]: " + std::get<ParseError>(result).message);
}
