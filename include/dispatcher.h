#pragma once

#include <expected>
#include <string>
#include <variant>
#include <vector>
#include "kvstore.h"
#include "command.h"
#include "wal.h"

struct Void {};

class Dispatcher {
    private:
        KVStore& kvstore_;
        WAL& wal_;
    public:
        Dispatcher(KVStore& kvstore, WAL& wal): kvstore_(kvstore), wal_(wal) {};
        using ResultValue = std::variant<std::monostate, Void, bool, int, std::string, std::vector<std::string>>;
        using DispatchResult = std::expected<ResultValue, std::string>;
        DispatchResult dispatch(const Command& command);

};