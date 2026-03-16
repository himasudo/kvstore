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
