#include "encoder.h"
#include "dispatcher.h"
#include <string>
#include <vector>
#include <variant>
#include <type_traits>

std::string encode(const Dispatcher::DispatchResult& result) {
    if (!result) {
        const std::string& err_msg = result.error(); 
        return "e" + std::to_string(err_msg.size()) + "\r\n" + err_msg + "\r\n";
    }
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Void>) { 
            // SET, CLEAR
            return "v\r\n";
        }
        else if constexpr (std::is_same_v<T, std::monostate>) {
            // GET (key not present)
            return "n\r\n";
        }
        else if constexpr (std::is_same_v<T, bool>) {
            // EXISTS, DEL
            return std::string("b") + (v ? "1": "0") + "\r\n";
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            return "s" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
        }
        else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            // KEYS
            std::string keys_str = ""; 
            for (const auto& val: v) {
                keys_str += std::to_string(val.size()) + "\r\n" + val + "\r\n";
            }
            return "l" + std::to_string(v.size()) + "\r\n" + keys_str;
        }
        else if constexpr (std::is_same_v<T, int>) {
            return "i" + std::to_string(std::to_string(v).size()) + "\r\n" + std::to_string(v) + "\r\n"; 
        }
        else {
            std::string encode_err = "Error[Encoder Error]: Unknown Type";
            return std::string("e") + std::to_string(encode_err.size()) + "\r\n" + encode_err + "\r\n";
        }

    }, result.value());
}
