#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <variant>
#include <type_traits>

#include "parser.h"
#include "kvstore.h"
#include "dispatcher.h"
#include "encoder.h"
#include "wal.h"

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
        
        // Uncommented the Void check since you added it to your encoder!
        if constexpr (std::is_same_v<T, Void>) {
            std::cout << "(Void / Success)\n";
        } else if constexpr (std::is_same_v<T, std::monostate>) {
            std::cout << "(Null / Not Found)\n";
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
 * Helper to run the parser, dispatcher, and encoder pipeline.
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
    std::cout << "Raw Input:       " << visual_input << "\n";
    
    // Step 1: Parse
    auto parse_result = parse(input);

    if (!parse_result) {
        std::cout << "Parser Status:   FAILURE\n";
        std::cout << "Parser Reason:   " << parse_result.error() << "\n";
        std::cout << "-------------------------------------------\n\n";
        return; 
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

    // Step 3: Encode
    std::string encoded_output = encode(dispatch_result);

    // Escape \r\n in the encoder output for easy reading in the console
    std::string visual_encoded = encoded_output;
    size_t pos_enc = 0;
    while ((pos_enc = visual_encoded.find("\r\n", pos_enc)) != std::string::npos) {
        visual_encoded.replace(pos_enc, 2, "\\r\\n");
        pos_enc += 4;
    }

    std::cout << "Encoder Output:  " << visual_encoded << "\n";
    std::cout << "-------------------------------------------\n\n";
}

int main() {
    std::cout << "--- Starting KVStore Pipeline Verification ---\n\n";

    KVStore kvstore;
    WAL wal;
    Dispatcher dispatcher(kvstore, wal);

    // --- 1. POPULATE STORE ---
    
    // SET foo bar → +OK\r\n
    run_pipeline_test("Valid SET foo bar", "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", dispatcher);
    
    // SET hello world → +OK\r\n
    run_pipeline_test("Valid SET hello world", "*3\r\n$3\r\nSET\r\n$5\r\nhello\r\n$5\r\nworld\r\n", dispatcher);

    // --- 2. READ OPERATIONS ---

    // GET foo → $3\r\nbar\r\n
    run_pipeline_test("Valid GET foo", "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", dispatcher);
    
    // GET fee (missing) → $-1\r\n
    run_pipeline_test("Valid GET fee (Not Found)", "*2\r\n$3\r\nGET\r\n$3\r\nfee\r\n", dispatcher);

    // EXISTS foo → :1\r\n
    run_pipeline_test("Valid EXISTS foo (True)", "*2\r\n$6\r\nEXISTS\r\n$3\r\nfoo\r\n", dispatcher);
    
    // EXISTS fake → :0\r\n
    run_pipeline_test("Valid EXISTS fake (False)", "*2\r\n$6\r\nEXISTS\r\n$4\r\nfake\r\n", dispatcher);

    // SIZE → :2\r\n
    run_pipeline_test("Valid SIZE (Expect 2)", "*1\r\n$4\r\nSIZE\r\n", dispatcher);

    // KEYS → *2\r\n$3\r\nfoo\r\n$5\r\nhello\r\n
    run_pipeline_test("Valid KEYS", "*1\r\n$4\r\nKEYS\r\n", dispatcher);

    // --- 3. DELETE & CLEAR OPERATIONS ---

    // DEL foo → :1\r\n
    run_pipeline_test("Valid DEL foo (Success)", "*2\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n", dispatcher);
    
    // DEL foo again → :0\r\n
    run_pipeline_test("Valid DEL foo (Already Deleted)", "*2\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n", dispatcher);

    // CLEAR → +OK\r\n
    run_pipeline_test("Valid CLEAR", "*1\r\n$5\r\nCLEAR\r\n", dispatcher);
    
    // SIZE after CLEAR → :0\r\n
    run_pipeline_test("Valid SIZE (Expect 0 after CLEAR)", "*1\r\n$4\r\nSIZE\r\n", dispatcher);

    // --- 4. DISPATCHER ERROR CASES ---

    // SET missing value → -ERR ...\r\n
    run_pipeline_test("Dispatcher Error: SET missing value", "*2\r\n$3\r\nSET\r\n$3\r\nfoo\r\n", dispatcher);

    // GET missing key → -ERR ...\r\n
    run_pipeline_test("Dispatcher Error: GET missing key", "*1\r\n$3\r\nGET\r\n", dispatcher);

    // --- 5. PARSER ERROR CASES ---

    run_pipeline_test("Parser Error: Missing * prefix", "3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", dispatcher);
    run_pipeline_test("Parser Error: Unknown command (MAGIC)", "*1\r\n$5\r\nMAGIC\r\n", dispatcher);

    return 0;
}