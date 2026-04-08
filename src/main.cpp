#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include "parser.h"
#include "kvstore.h"

/**
 * Converts the Command::Type enum to a string for display.
 * This is used to verify that the parser correctly identified the command.
 */
std::string commandTypeToString(Command::Type type) {
    switch (type) {
        case Command::Type::SET:    return "SET";
        case Command::Type::GET:    return "GET";
        case Command::Type::KEYS:   return "KEYS";
        case Command::Type::SIZE:   return "SIZE";
        case Command::Type::EXISTS: return "EXISTS";
        case Command::Type::DEL:    return "DEL";
        case Command::Type::CLEAR:  return "CLEAR";
        default:                    return "UNKNOWN_COMMAND";
    }
}

/**
 * Helper to run the parser against a raw protocol string and print results.
 */
void run_parser_test(const std::string& name, std::string_view input) {
    std::cout << ">>> Test: " << name << " <<<\n";
    
    // Formatting the input string so \r\n is visible in the console output
    std::string visual_input(input);
    size_t pos = 0;
    while ((pos = visual_input.find("\r\n", pos)) != std::string::npos) {
        visual_input.replace(pos, 2, "\\r\\n");
        pos += 4;
    }
    std::cout << "Raw Input: " << visual_input << "\n";
    
    auto result = parse(input);

    if (result) {
        std::cout << "Status:  SUCCESS\n";
        std::cout << "Command: " << commandTypeToString(result->type) << "\n";
        std::cout << "Args:    ";
        if (result->args.empty()) {
            std::cout << "(none)";
        } else {
            for (const auto& arg : result->args) {
                std::cout << "[" << arg << "] ";
            }
        }
        std::cout << "\n";
    } else {
        std::cout << "Status:  FAILURE\n";
        std::cout << "Reason:  " << result.error() << "\n";
    }
    std::cout << "-------------------------------------------\n\n";
}

int main() {
    std::cout << "--- Starting KVStore Protocol Parser Verification ---\n\n";

    // Valid: SET foo bar (*3 tokens: SET, foo, bar)
    run_parser_test("Valid SET foo bar", "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");

    // Valid: GET foo (*2 tokens: GET, foo)
    run_parser_test("Valid GET foo", "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");

    // Valid: SIZE (*1 token: SIZE)
    run_parser_test("Valid SIZE", "*1\r\n$4\r\nSIZE\r\n");

    // Invalid: Missing * prefix
    run_parser_test("Missing * prefix", "3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");

    // Invalid: Wrong arg count (*3 declared, but only 2 tokens provided)
    run_parser_test("Wrong arg count (*3 but only 2 found)", "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n");

    // Invalid: Unknown command string
    run_parser_test("Unknown command (MAGIC)", "*1\r\n$5\r\nMAGIC\r\n");

    return 0;
}