#include "encoder.h"
#include "dispatcher.h"
#include <string>
#include <vector>
#include <variant>
#include <type_traits>

std::string encode(const Dispatcher::DispatchResult& result) {
    if (!result) {
        return "-ERR " + result.error() + "\r\n";
    }
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Void>) {
            return "+OK\r\n";
        }
        else if constexpr (std::is_same_v<T, std::monostate>) {
            return "$-1\r\n";
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return v ? ":1\r\n" : ":0\r\n";
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            return "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
        }
        else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            std::string out = "*" + std::to_string(v.size()) + "\r\n";
            for (const auto& s : v)
                out += "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
            return out;
        }
        else if constexpr (std::is_same_v<T, int>) {
            return ":" + std::to_string(v) + "\r\n";
        }
        else {
            return "-ERR unknown type\r\n";
        }
    }, result.value());
}
