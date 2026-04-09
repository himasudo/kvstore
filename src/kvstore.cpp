#include "kvstore.h"


void KVStore::set(std::string key, std::string value) {
    store_.insert_or_assign(std::move(key), std::move(value));
}

std::optional<std::string> KVStore::get(std::string_view key) const {
    auto it = store_.find(key);
    if (it != store_.end()) {
        return it->second;
    }
    return std::nullopt;
}

size_t KVStore::size() const {
    return store_.size();
}

bool KVStore::del(std::string_view key) {
    auto it = store_.find(key);
    if (it != store_.end()) {
        store_.erase(it);
        return true;
    }
    return false;
}

bool KVStore::exists(std::string_view key) const {
     return store_.contains(key);
}

std::vector<std::string> KVStore::keys() const {
    std::vector<std::string> store_keys;
    store_keys.reserve(store_.size());
    for(const auto& pair: store_) {
        store_keys.push_back(pair.first);
    }
    return store_keys;
}

void KVStore::clear() {
    store_.clear();
}