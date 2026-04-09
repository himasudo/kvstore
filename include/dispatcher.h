#pragma once

#include <expected>
#include <string>
#include <variant>
#include <vector>
#include "kvstore.h"
#include "command.h"


class Dispatcher {
    private:
        KVStore& kvstore_;
    public:
        Dispatcher(KVStore& kvstore): kvstore_(kvstore) {};
        using ResultValue = std::variant<std::monostate, bool, int, std::string, std::vector<std::string>>;
        using DispatchResult = std::expected<ResultValue, std::string>;
        DispatchResult dispatch(const Command& command);

};