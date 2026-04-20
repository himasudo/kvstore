// Entry point for the kvstore server.
// Initializes the KVStore and starts the TCP server on a configured port.

#include "kvstore.h"
#include "dispatcher.h"
#include "server.h"

int main() {
    KVStore store;
    Dispatcher dispatcher(store);
    return run_server(dispatcher);
}