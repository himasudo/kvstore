#include <iostream>
#include "kvstore.h"

int main() {
    std::cout << "kvstore starting ...\n";
    KVStore store;
    store.set("Himanshu", "online");
    store.set("John", "online");
    store.set("Doe", "online");
    store.set("Mike", "online");

    std::cout << std::boolalpha;
    std::cout << "size of kvstore: " << store.size() << std::endl;
    bool val_exists = store.exists("Himanshu");
    std::cout << "Does key Himanshu exists??: " << val_exists << std::endl;
    auto result = store.get("Himanshu");
    if (result) {
        std::cout << "Himanshu status: " << *result << std::endl;
    } else {
        std::cout << "Himanshu: not found" << std::endl;
    }
    std::cout << "Store Keys count: " << store.keys().size() << std::endl;
    bool deleted_john = store.del("John");
    std::cout << "John deleted: " << deleted_john << std::endl;

    auto get_deleted_john = store.get("John");
    if (get_deleted_john) {
        std::cout << "Fetching deleted John: " << *get_deleted_john << std::endl;
    } else {
        std::cout << "John: not found" << std::endl;
    }
    std::cout << "Delete all mappings from store" << std::endl;
    store.clear();
    std::cout << "Keys in the store: " << store.keys().size() << std::endl;
    return 0;
}