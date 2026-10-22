#include "kvstore.h"
#include "dispatcher.h"
#include "server.h"
#include "wal.h"
#include "snapshot.h"
#include "data_lock.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#include <iostream>
#include <atomic>
#include <chrono>
#include <csignal>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

std::atomic<bool> keep_running{true};
int shutdown_fd = -1;

void request_shutdown() {
    keep_running = false;

    if (shutdown_fd != -1) {
        uint64_t wake_signal = 1;
        ssize_t res = write(shutdown_fd, &wake_signal, sizeof(wake_signal));
        (void)res;
    }
}

// Async-signal-safe: an atomic store and a write() to the eventfd.
void handle_shutdown_signal(int) {
    request_shutdown();
}

namespace {

// Lets main() wake the checkpoint thread at shutdown instead of waiting out its
// sleep. Not touched from the signal handler (condition variables are not
// async-signal-safe); main() notifies after the workers have stopped.
std::mutex snapshot_wait_mutex;
std::condition_variable snapshot_wait_cv;

void wake_snapshot_worker() {
    { std::lock_guard<std::mutex> lock(snapshot_wait_mutex); }
    snapshot_wait_cv.notify_all();
}

void snapshot_worker(Dispatcher& dispatcher) {
    Snapshot snapshot;
    const auto interval = std::chrono::seconds(60);

    std::unique_lock<std::mutex> lock(snapshot_wait_mutex);
    while (keep_running) {
        if (snapshot_wait_cv.wait_for(lock, interval, [] { return !keep_running.load(); })) {
            break;
        }

        lock.unlock();
        try {
            std::cout << "[Background] Taking snapshot..." << std::endl;
            dispatcher.checkpoint(snapshot, "kvstore.snapshot");
            std::cout << "[Background] Snapshot successful. WAL truncated." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Background Error] " << e.what() << std::endl;
        }
        lock.lock();
    }
}

int run() {
    shutdown_fd = eventfd(0, EFD_NONBLOCK);
    if (shutdown_fd == -1) {
        std::cerr << "Failed to create eventfd" << std::endl;
        return 1;
    }

    // SIGINT and SIGTERM both request a clean shutdown (docker stop and
    // systemd send SIGTERM). A write to a closed pipe or socket must surface as
    // an error return, not kill the process.
    struct sigaction shutdown_action{};
    shutdown_action.sa_handler = handle_shutdown_signal;
    sigemptyset(&shutdown_action.sa_mask);
    if (sigaction(SIGINT, &shutdown_action, nullptr) == -1 ||
        sigaction(SIGTERM, &shutdown_action, nullptr) == -1) {
        std::cerr << "Failed to install signal handlers" << std::endl;
        return 1;
    }
    std::signal(SIGPIPE, SIG_IGN);

    // Must come before any data file is opened: a second instance sharing the
    // WAL and snapshot would corrupt them (SO_REUSEPORT lets it bind the port).
    std::unique_ptr<DataDirLock> data_lock;
    try {
        data_lock = std::make_unique<DataDirLock>("kvstore.lock");
    } catch (const std::exception& e) {
        std::cerr << "kvstore: cannot start: " << e.what() << std::endl;
        return 1;
    }

    KVStore store;
    Snapshot snapshot;
    
    std::cout << "Booting Key-Value Server..." << std::endl;

    try {
        Snapshot::discard_stale_temp("kvstore.snapshot");
        std::cout << "Loading snapshot..." << std::endl;
        snapshot.recover(store, "kvstore.snapshot");
        std::cout << "Snapshot loaded. Current store size: " << store.size() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Snapshot recovery failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Initializing Write-Ahead Log..." << std::endl;
    std::unique_ptr<WAL> wal_ptr;
    std::vector<Command> recovered_commands;
    try {
        wal_ptr = std::make_unique<WAL>("kvstore.wal");
        recovered_commands = wal_ptr->recover();
    } catch (const std::exception& e) {
        std::cerr << "WAL recovery failed: " << e.what()
                  << "\nkvstore.wal was left unchanged; refusing to start." << std::endl;
        return 1;
    }
    WAL& wal = *wal_ptr;
    if (wal.truncated_tail_bytes() != 0) {
        std::cerr << "WAL: discarded " << wal.truncated_tail_bytes()
                  << " bytes of torn tail from an interrupted write." << std::endl;
    }

    if (!recovered_commands.empty()) {
        std::cout << "Replaying " << recovered_commands.size() << " commands from WAL directly to store..." << std::endl;
        for (const auto& cmd : recovered_commands) {
            if (cmd.type == Command::Type::SET && cmd.args.size() == 2) {
                store.set(cmd.args[0], cmd.args[1]);
            } 
            else if (cmd.type == Command::Type::DEL && !cmd.args.empty()) {
                store.del(cmd.args[0]);
            }
            else if (cmd.type == Command::Type::CLEAR) {
                store.clear();
            }
        }
        std::cout << "WAL replay complete." << std::endl;
    } else {
        std::cout << "WAL is empty." << std::endl;
    }

    Dispatcher dispatcher(store, wal);

    std::cout << "Starting background snapshot thread (60s interval)..." << std::endl;
    std::thread snap_thread(snapshot_worker, std::ref(dispatcher));

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::cout << "Spinning up " << num_threads << " network worker threads..." << std::endl;
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    std::atomic<bool> worker_failed{false};
    try {
        for (unsigned int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&] {
                if (run_server(dispatcher) != 0) {
                    if (!worker_failed.exchange(true)) {
                        std::cerr << "Network worker failed; shutting down server." << std::endl;
                        request_shutdown();
                    }
                }
            });
        }
    } catch (const std::exception& e) {
        // Could not start every worker. Stop the ones that did start so no
        // joinable thread is destroyed, then report through main().
        request_shutdown();
        for (auto& t : threads) t.join();
        wake_snapshot_worker();
        snap_thread.join();
        throw;
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    
    wake_snapshot_worker();
    if (snap_thread.joinable()) {
        snap_thread.join();
    }

    // No client can write any more, so a final checkpoint leaves an empty WAL
    // and the next start has nothing to replay. If it fails the WAL is still
    // complete and valid, so this is a warning, not a failed shutdown.
    if (!worker_failed) {
        try {
            std::cout << "Taking final snapshot..." << std::endl;
            Snapshot final_snapshot;
            dispatcher.checkpoint(final_snapshot, "kvstore.snapshot");
            std::cout << "Final snapshot written. WAL truncated." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Final snapshot failed (WAL left intact): " << e.what() << std::endl;
        }
    }

    if (shutdown_fd != -1) close(shutdown_fd);
    std::cout << "Server completely shut down." << std::endl;
    return worker_failed ? 1 : 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << "kvstore: fatal error: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "kvstore: fatal error: unknown exception" << std::endl;
    }
    return 1;
}
