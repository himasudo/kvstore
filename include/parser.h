#pragma once
#include <string>
#include <string_view>
#include <expected>
#include "command.h"

std::expected<Command, std::string> parse(std::string_view input);