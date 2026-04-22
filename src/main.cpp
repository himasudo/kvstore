#include "kvstore.h"
#include "dispatcher.h"
#include "server.h"
#include "wal.h"
#include "snapshot.h"

#include <thread>
#include <vector>
#include <functional>
#include <iostream>
#include <atomic>
#include <chrono>
#include <csignal>

std::atomic<bool> keep_running{true};

void handle_sigint(int) {
    std::cout << "\nInitiating graceful shutdown..." << std::endl;
    keep_running = false;
}

void snapshot_worker(KVStore& store, WAL& wal) {
    Snapshot snapshot;
    const int interval_seconds = 60;

    while (keep_running) {
        for (int i = 0; i < interval_seconds && keep_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (!keep_running) break;

        try {
            std::cout << "[Background] Taking snapshot..." << std::endl;
            snapshot.write(store, "kvstore.snapshot");
            
            wal.reset();
            std::cout << "[Background] Snapshot successful. WAL truncated." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Background Error] " << e.what() << std::endl;
        }
    }
}

int main() {
    std::signal(SIGINT, handle_sigint);

    KVStore store;
    Snapshot snapshot;
    
    std::cout << "Booting Key-Value Server..." << std::endl;

    try {
        std::cout << "Loading snapshot..." << std::endl;
        snapshot.recover(store, "kvstore.snapshot");
        std::cout << "Snapshot loaded. Current store size: " << store.size() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Snapshot recovery failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Initializing Write-Ahead Log..." << std::endl;
    WAL wal("kvstore.wal");
    std::vector<Command> recovered_commands = wal.recover();

    if (!recovered_commands.empty()) {
        std::cout << "Replaying " << recovered_commands.size() << " commands from WAL directly to store..." << std::endl;
        for (const auto& cmd : recovered_commands) {
            if (cmd.type == Command::Type::SET && cmd.args.size() == 2) {
                store.set(cmd.args[0], cmd.args[1]);
            } 
            else if (cmd.type == Command::Type::DEL && !cmd.args.empty()) {
                store.del(cmd.args[0]);
            }
        }
        std::cout << "WAL replay complete." << std::endl;
    } else {
        std::cout << "WAL is empty." << std::endl;
    }

    Dispatcher dispatcher(store, wal);

    std::cout << "Starting background snapshot thread (60s interval)..." << std::endl;
    std::thread snap_thread(snapshot_worker, std::ref(store), std::ref(wal));

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::cout << "Spinning up " << num_threads << " network worker threads..." << std::endl;
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (unsigned int i = 0; i < num_threads; ++i) {
        threads.emplace_back(run_server, std::ref(dispatcher));
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    
    if (snap_thread.joinable()) {
        snap_thread.join();
    }

    std::cout << "Server completely shut down." << std::endl;
    return 0;
}