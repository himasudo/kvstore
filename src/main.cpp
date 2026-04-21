// Entry point for the kvstore server.
// Initializes the KVStore and starts the TCP server on a configured port.

#include "kvstore.h"
#include "dispatcher.h"
#include "server.h"

#include <thread>
#include <vector>
#include <functional>
#include <iostream>

int main() {
    KVStore store;

    std::cout << "Initializing Write-Ahead Log..." << std::endl;
    WAL wal("kvstore.wal");

    Dispatcher dispatcher(store, wal);

    std::cout << "Recovering from WAL..." << std::endl;
    std::vector<Command> recovered_commands = wal.recover();

    if (!recovered_commands.empty()) {
        std::cout << "Replaying " << recovered_commands.size() << " commands from WAL..." << std::endl;
        for (const auto& cmd : recovered_commands) {
            if (cmd.type == Command::Type::SET && cmd.args.size() == 2) {
                store.set(cmd.args[0], cmd.args[1]);
            } 
            else if (cmd.type == Command::Type::DEL && !cmd.args.empty()) {
                store.del(cmd.args[0]);
            }
        }
        std::cout << "Recovery complete." << std::endl;
    } else {
        std::cout << "WAL is empty. Starting fresh." << std::endl;
    }


    unsigned int num_threads = std::thread::hardware_concurrency();
    
    if (num_threads == 0) {
        num_threads = 4; // fallback
    }

    std::cout << "Spinning up " << num_threads << " worker threads..." << std::endl;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (unsigned int i = 0; i < num_threads; ++i) {
        threads.emplace_back(run_server, std::ref(dispatcher));
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    return 0;
}