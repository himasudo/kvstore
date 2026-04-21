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
    Dispatcher dispatcher(store);
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