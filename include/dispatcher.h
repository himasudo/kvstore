#pragma once

#include <expected>
#include <mutex>
#include <string>
#include <variant>
#include <vector>
#include "kvstore.h"
#include "command.h"
#include "wal.h"

class Snapshot;

struct Void {};

class Dispatcher {
    private:
        KVStore& kvstore_;
        WAL& wal_;
        std::mutex mutation_mutex_;
    public:
        Dispatcher(KVStore& kvstore, WAL& wal): kvstore_(kvstore), wal_(wal) {};
        using ResultValue = std::variant<std::monostate, Void, bool, int, std::string, std::vector<std::string>>;
        using DispatchResult = std::expected<ResultValue, std::string>;
        DispatchResult dispatch(const Command& command);
        void checkpoint(Snapshot& snapshot, const std::string& path);

};
