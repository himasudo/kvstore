#include <algorithm>
#include <barrier>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dispatcher.h"
#include "kvstore.h"
#include "snapshot.h"
#include "wal.h"

static void replay(KVStore& store, const std::vector<Command>& commands) {
    for (const auto& cmd : commands) {
        switch (cmd.type) {
            case Command::Type::SET:
                store.set(cmd.args[0], cmd.args[1]);
                break;
            case Command::Type::DEL:
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

int main() {
    constexpr int rounds = 8;
    constexpr int writer_count = 6;
    constexpr int operations_per_writer = 10;

    for (int round = 0; round < rounds; ++round) {
        const auto base = std::filesystem::temp_directory_path() /
            ("kvstore-checkpoint-" + std::to_string(getpid()) + "-" + std::to_string(round));
        const auto wal_path = base.string() + ".wal";
        const auto snapshot_path = base.string() + ".snapshot";

        std::remove(wal_path.c_str());
        std::remove(snapshot_path.c_str());
        std::remove((snapshot_path + ".tmp").c_str());

        KVStore live_store;
        {
            WAL wal(wal_path);
            Dispatcher dispatcher(live_store, wal);
            Snapshot snapshot;

            for (int i = 0; i < 12; ++i) {
                dispatcher.dispatch(Command{Command::Type::SET,
                    {"seed-" + std::to_string(i), "initial"}});
            }

            std::barrier start(writer_count + 1);
            std::vector<std::thread> writers;
            writers.reserve(writer_count);

            for (int writer = 0; writer < writer_count; ++writer) {
                writers.emplace_back([&, writer] {
                    start.arrive_and_wait();
                    for (int operation = 0; operation < operations_per_writer; ++operation) {
                        const std::string key = "key-" + std::to_string(operation % 4);
                        const std::string value = std::to_string(writer) + ":" + std::to_string(operation);

                        if ((writer + operation) % 5 == 0) {
                            dispatcher.dispatch(Command{Command::Type::DEL, {key}});
                        } else {
                            dispatcher.dispatch(Command{Command::Type::SET, {key, value}});
                        }
                    }
                });
            }

            std::thread checkpoint_thread([&] {
                start.arrive_and_wait();
                dispatcher.checkpoint(snapshot, snapshot_path);
            });

            for (auto& writer : writers) {
                writer.join();
            }
            checkpoint_thread.join();
        }

        KVStore recovered_store;
        Snapshot snapshot;
        snapshot.recover(recovered_store, snapshot_path);
        {
            WAL wal(wal_path);
            replay(recovered_store, wal.recover());
        }

        assert(sorted_entries(live_store) == sorted_entries(recovered_store));

        std::remove(wal_path.c_str());
        std::remove(snapshot_path.c_str());
        std::remove((snapshot_path + ".tmp").c_str());
    }

    return 0;
}
