#pragma once
#include <string>
#include <string_view>
#include <expected>
#include "command.h"

inline constexpr size_t MAX_KEY_SIZE    = 256;
inline constexpr size_t MAX_VALUE_SIZE  = 1 * 1024 * 1024;
inline constexpr size_t MAX_BUFFER_SIZE = MAX_VALUE_SIZE + 512;
inline constexpr size_t MAX_COMMAND_SIZE = 16;
inline constexpr int MAX_ARG_COUNT = 2;

std::expected<Command, std::string> parse(std::string_view input);