#include <algorithm>
#include <barrier>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

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

static auto sorted_entries(const KVStore& store) {
    auto entries = store.entries();
    std::sort(entries.begin(), entries.end());
    return entries;
}

int main(int argc, char** argv) {
    int rounds = 8;
    if (argc == 2) {
        rounds = std::atoi(argv[1]);
        assert(rounds > 0);
    }

    constexpr int thread_count = 8;
    constexpr int operations_per_thread = 8;

    for (int round = 0; round < rounds; ++round) {
        const auto path = std::filesystem::temp_directory_path() /
            ("kvstore-ordering-" + std::to_string(getpid()) + "-" + std::to_string(round) + ".wal");

        std::remove(path.c_str());

        KVStore live_store;
        {
            WAL wal(path.string());
            Dispatcher dispatcher(live_store, wal);
            std::barrier start(thread_count);
            std::vector<std::thread> workers;
            workers.reserve(thread_count);

            for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
                workers.emplace_back([&, thread_id] {
                    start.arrive_and_wait();

                    for (int operation = 0; operation < operations_per_thread; ++operation) {
                        const std::string value =
                            std::to_string(thread_id) + ":" + std::to_string(operation);

                        auto result = dispatcher.dispatch(
                            Command{Command::Type::SET, {"shared", value}}
                        );
                        assert(result.has_value());
                    }
                });
            }

            for (auto& worker : workers) {
                worker.join();
            }
        }

        KVStore recovered_store;
        {
            WAL wal(path.string());
            auto commands = wal.recover();

            assert(commands.size() ==
                   static_cast<size_t>(thread_count * operations_per_thread));

            replay(recovered_store, commands);
        }

        assert(sorted_entries(live_store) == sorted_entries(recovered_store));
        std::remove(path.c_str());
    }

    return 0;
}
