#pragma once

#include <string>
#include "kvstore.h"

class Snapshot {
    public:
        void write(const KVStore& store, const std::string& path);
        void recover(KVStore& store, const std::string& path);
};