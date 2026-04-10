#include "dispatcher.h"

Dispatcher::DispatchResult Dispatcher::dispatch(const Command& command) {
    switch(command.type) {
        case Command::Type::SET:
            if (command.args.size() != 2) return std::unexpected("Error[Dispatcher Error]: SET requires a key and a value argument");
            kvstore_.set(command.args[0], command.args[1]);
            return Void{};

        case Command::Type::GET: {
            if (command.args.empty()) return std::unexpected("Error[Dispatcher Error]: GET requires a key");
            auto get_result = kvstore_.get(command.args[0]);
            if (get_result) {
                return *get_result;
            } else {
                return std::monostate{};
            }
        }
        
        case Command::Type::KEYS:
            return kvstore_.keys();

        case Command::Type::SIZE:
            return static_cast<int>(kvstore_.size());

        case Command::Type::EXISTS:
            if (command.args.empty()) return std::unexpected("Error[Dispatcher Error]: EXISTS requires a key");
            return kvstore_.exists(command.args[0]);

        case Command::Type::DEL:
            if (command.args.empty()) return std::unexpected("Error[Dispatcher Error]: DEL requires a key");
            return kvstore_.del(command.args[0]);

        case Command::Type::CLEAR:
            kvstore_.clear();
            return Void{};

        default:
            return std::unexpected("Error[Dispatcher Error] Unknown command type");
    }
}