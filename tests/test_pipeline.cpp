#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <variant>
#include <type_traits>

#include "parser.h"
#include "kvstore.h"
#include "dispatcher.h"

/**
 * Converts the Command::Type enum to a string for display.
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
 * Helper to print the std::variant inside the DispatchResult using std::visit.
 */
void printDispatchResultData(const Dispatcher::DispatchResult::value_type& val) {
    std::visit([](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        
        if constexpr (std::is_same_v<T, std::monostate>) {
            std::cout << "(OK / Empty)\n";
        } else if constexpr (std::is_same_v<T, bool>) {
            std::cout << (v ? "true" : "false") << "\n";
        } else if constexpr (std::is_same_v<T, int>) {
            std::cout << v << "\n";
        } else if constexpr (std::is_same_v<T, std::string>) {
            std::cout << "\"" << v << "\"\n";
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            std::cout << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                std::cout << "\"" << v[i] << "\"" << (i + 1 < v.size() ? ", " : "");
            }
            std::cout << "]\n";
        }
    }, val);
}

/**
 * Helper to run the parser and dispatcher pipeline.
 */
void run_pipeline_test(const std::string& name, std::string_view input, Dispatcher& dispatcher) {
    std::cout << ">>> Test: " << name << " <<<\n";
    
    // Formatting the input string so \r\n is visible in the console output
    std::string visual_input(input.data(), input.size());
    size_t pos = 0;
    while ((pos = visual_input.find("\r\n", pos)) != std::string::npos) {
        visual_input.replace(pos, 2, "\\r\\n");
        pos += 4;
    }
    std::cout << "Raw Input: " << visual_input << "\n";
    
    // Step 1: Parse
    auto parse_result = parse(input);

    if (!parse_result) {
        std::cout << "Parser Status:   FAILURE\n";
        std::cout << "Parser Reason:   " << parse_result.error() << "\n";
        std::cout << "-------------------------------------------\n\n";
        return; // Stop here if parsing fails
    }

    std::cout << "Parser Status:   SUCCESS (" << commandTypeToString(parse_result->type) << ")\n";

    // Step 2: Dispatch
    auto dispatch_result = dispatcher.dispatch(*parse_result);

    if (!dispatch_result) {
        std::cout << "Dispatch Status: FAILURE\n";
        std::cout << "Dispatch Reason: " << dispatch_result.error() << "\n";
    } else {
        std::cout << "Dispatch Status: SUCCESS\n";
        std::cout << "Result Data:     ";
        printDispatchResultData(dispatch_result.value());
    }
    
    std::cout << "-------------------------------------------\n\n";
}

int main() {
    std::cout << "--- Starting KVStore Pipeline Verification ---\n\n";

    // Initialize our core components
    KVStore kvstore;
    Dispatcher dispatcher(kvstore);

    // --- SUCCESS CASES ---
    
    // Valid: SET foo bar
    run_pipeline_test("Valid SET foo bar", "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", dispatcher);

    // Valid: GET foo (Should return "bar" now that we set it!)
    run_pipeline_test("Valid GET foo", "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", dispatcher);

    // Valid: SIZE (Should return 1)
    run_pipeline_test("Valid SIZE", "*1\r\n$4\r\nSIZE\r\n", dispatcher);

    // --- DISPATCHER ERROR CASES ---

    // Invalid Dispatch: SET with only 1 argument (*2 tokens: SET, foo)
    run_pipeline_test("Dispatcher Error: SET missing value", "*2\r\n$3\r\nSET\r\n$3\r\nfoo\r\n", dispatcher);

    // Invalid Dispatch: GET missing key (*1 token: GET)
    run_pipeline_test("Dispatcher Error: GET missing key", "*1\r\n$3\r\nGET\r\n", dispatcher);

    // --- PARSER ERROR CASES ---

    // Invalid Parse: Missing * prefix
    run_pipeline_test("Parser Error: Missing * prefix", "3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", dispatcher);

    // Invalid Parse: Unknown command string
    run_pipeline_test("Parser Error: Unknown command (MAGIC)", "*1\r\n$5\r\nMAGIC\r\n", dispatcher);

    return 0;
}