#include "kvstore.h"
#include <shared_mutex>

void KVStore::set(std::string key, std::string value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    store_.insert_or_assign(std::move(key), std::move(value));
}

std::optional<std::string> KVStore::get(std::string_view key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        return it->second;
    }
    return std::nullopt;
}

size_t KVStore::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return store_.size();
}

bool KVStore::del(std::string_view key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        store_.erase(it);
        return true;
    }
    return false;
}

bool KVStore::exists(std::string_view key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return store_.contains(key);
}

std::vector<std::string> KVStore::keys() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::string> store_keys;
    store_keys.reserve(store_.size());
    for(const auto& pair: store_) {
        store_keys.push_back(pair.first);
    }
    return store_keys;
}

void KVStore::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    store_.clear();
}

std::vector<std::pair<std::string, std::string>> KVStore::entries() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::string>> store_entries;
    store_entries.reserve(store_.size());

    for (const auto& pair : store_) {
        store_entries.push_back(pair);
    }

    return store_entries;
}