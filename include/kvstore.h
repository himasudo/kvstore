#pragma once

#include <string>
#include <optional>
#include <unordered_map>
#include <vector>

class KVStore {
    public:
        void set(const std::string &key, const std::string &value);
        
        std::optional<std::string> get(const std::string &key) const;
        
        bool del(const std::string &key);

        size_t size() const;

        bool exists(const std::string &key) const;

        std::vector<std::string> keys() const;

        void clear();

    private:
        std::unordered_map<std::string, std::string> store_;
};