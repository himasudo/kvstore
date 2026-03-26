#include "kvstore.h"


void KVStore::set(const std::string &key, const std::string &value) {
    store_[key] = value;
}

std::optional<std::string> KVStore::get(const std::string &key) const {
    auto it = store_.find(key);
    if (it != store_.end()) {
        return it->second;
    }
    return std::nullopt;
}

size_t KVStore::size() const {
    return store_.size();
}

bool KVStore::del(const std::string &key) {
    return store_.erase(key) > 0;
}

bool KVStore::exists(const std::string &key) const {
    auto it = store_.find(key);
    return it != store_.end();
}

std::vector<std::string> KVStore::keys() const {
    std::vector<std::string> store_keys;
    store_keys.reserve(store_.size());
    for(const auto &pair: store_) {
        store_keys.push_back(pair.first);
    }
    return store_keys;
}

void KVStore::clear() {
    store_.clear();
}