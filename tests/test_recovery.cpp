#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "dispatcher.h"
#include "kvstore.h"
#include "wal.h"

static void replay(KVStore& store, const std::vector<Command>& commands) {
    for (const auto& cmd : commands) {
        switch (cmd.type) {
            case Command::Type::SET:
                assert(cmd.args.size() == 2);
                store.set(cmd.args[0], cmd.args[1]);
                break;
            case Command::Type::DEL:
                assert(!cmd.args.empty());
                store.del(cmd.args[0]);
                break;
            case Command::Type::CLEAR:
                store.clear();
                break;
            default:
                assert(false && "unexpected command in WAL");
        }
    }
}

int main() {
    const std::string path = "/tmp/kvstore-recovery-" + std::to_string(getpid()) + ".wal";
    std::remove(path.c_str());

    {
        KVStore store;
        WAL wal(path);
        Dispatcher dispatcher(store, wal);

        dispatcher.dispatch(Command{Command::Type::SET, {"before-clear", "one"}});
        dispatcher.dispatch(Command{Command::Type::SET, {"also-before-clear", "two"}});
        dispatcher.dispatch(Command{Command::Type::CLEAR, {}});
        dispatcher.dispatch(Command{Command::Type::SET, {"after-clear", "three"}});

        assert(store.size() == 1);
        assert(store.get("after-clear") == std::optional<std::string>{"three"});
    }

    KVStore recovered_store;
    {
        WAL wal(path);
        auto commands = wal.recover();

        assert(commands.size() == 4);
        assert(commands[0].type == Command::Type::SET);
        assert(commands[1].type == Command::Type::SET);
        assert(commands[2].type == Command::Type::CLEAR);
        assert(commands[3].type == Command::Type::SET);

        replay(recovered_store, commands);
    }

    assert(recovered_store.size() == 1);
    assert(!recovered_store.exists("before-clear"));
    assert(!recovered_store.exists("also-before-clear"));
    assert(recovered_store.get("after-clear") == std::optional<std::string>{"three"});

    std::remove(path.c_str());
    return 0;
}
