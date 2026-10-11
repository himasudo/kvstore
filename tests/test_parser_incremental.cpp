#include <cassert>
#include <string>
#include <string_view>
#include <variant>
#include <iostream>
#include <random>
#include "parser.h"

static const ParseComplete& complete(const ParseResult& result) {
    assert(std::holds_alternative<ParseComplete>(result));
    return std::get<ParseComplete>(result);
}
static void error(std::string_view wire) {
    auto r = parse_one(wire); if (!std::holds_alternative<ParseError>(r)) { std::cerr << "Expected error for [" << std::string(wire) << "] but got " << r.index() << "\n"; } assert(std::holds_alternative<ParseError>(r));
}
static void incomplete(std::string_view wire) {
    assert(std::holds_alternative<ParseIncomplete>(parse_one(wire)));
}

int main() {
    const std::string set = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    for (size_t n = 0; n < set.size(); ++n) incomplete(std::string_view(set).substr(0, n));

    auto result = parse_one(set);
    const auto& parsed = complete(result);
    assert(parsed.command.type == Command::Type::SET);
    assert(parsed.command.args.size() == 2);
    assert(parsed.command.args[0] == "foo" && parsed.command.args[1] == "bar");
    assert(parsed.consumed == set.size());

    const std::string get = "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n";
    const std::string pipelined = set + get;
    auto first = parse_one(pipelined);
    assert(complete(first).consumed == set.size());
    auto second = parse_one(std::string_view(pipelined).substr(complete(first).consumed));
    assert(complete(second).command.type == Command::Type::GET);
    assert(complete(second).consumed == get.size());
    assert(!parse(pipelined)); // legacy API must not discard an extra command
    assert(parse(set));

    const std::string val("a\r\n\0b", 5);
    const std::string binary = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\n" + val + "\r\n";
    auto binary_result = parse_one(binary);
    assert(complete(binary_result).command.args[1] == val);

    const std::string empty_value = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n";
    assert(complete(parse_one(empty_value)).command.args[1].empty());

    incomplete("");
    incomplete("*2\r");
    incomplete("*1\r\n$4\r\nSIZE\r");
    incomplete("*1\r\n$4\r\nSIZ");
    incomplete("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$2\r\nx");
    error("*0\r\n");
    error("*-1\r\n");
    error("*4\r\n");
    error("*999999999999999999999\r\n");
    error("*1\n");
    incomplete("*1\r\n$5\r\nSIZE\r\n");
    error("*1\r\n$5\r\nSIZEx\r\n");
    error("*1\r\n$4\r\nSIZE\nx");
    error("*1\r\n$5\r\nMAGIC\r\n");
    error("*1\r\n$17\r\n");
    error("*2\r\n$3\r\nGET\r\n$257\r\n");
    error("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1048577\r\n");
    error("*1\r\n$9999999999999999999\r\n");
    // Generated binary-safe SET commands, with fragmented and pipelined delivery.
    std::mt19937 rng(0x6e657477);
    for (int iteration = 0; iteration < 300; ++iteration) {
        auto make_bytes = [&](size_t n) {
            std::string data;
            data.reserve(n);
            for (size_t j = 0; j < n; ++j) {
                data.push_back(static_cast<char>(rng() & 255));
            }
            return data;
        };
        std::string key = make_bytes(rng() % 64);
        std::string value = make_bytes(rng() % 256);
        std::string wire = "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) +
            "\r\n" + key + "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
        auto parsed_wire = parse_one(wire + get);
        assert(complete(parsed_wire).consumed == wire.size());
        assert(complete(parsed_wire).command.args[0] == key);
        assert(complete(parsed_wire).command.args[1] == value);
        for (int j = 0; j < 12; ++j) {
            size_t cutoff = rng() % wire.size();
            incomplete(std::string_view(wire).substr(0, cutoff));
        }
    }

    // Maximum legal value, followed by an oversized advertised value.
    std::string big_value(MAX_VALUE_SIZE, 'v');
    std::string big_command = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$" +
        std::to_string(MAX_VALUE_SIZE) + "\r\n" + big_value + "\r\n";
    assert(complete(parse_one(big_command)).command.args[1].size() == MAX_VALUE_SIZE);
    return 0;
}
