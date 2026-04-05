#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>
#include <functional>

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const {
        return std::hash<std::string_view>{}(sv);
    }
};

class KVStore {
    public:
        void set(std::string key, std::string value);
        
        std::optional<std::string> get(std::string_view key) const;
        
        bool del(std::string_view key);

        size_t size() const;

        bool exists(std::string_view key) const;

        std::vector<std::string> keys() const;

        void clear();

    private:
        std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> store_;
};