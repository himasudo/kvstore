#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "kvstore.h"
#include "snapshot.h"
#include "wal.h"

static mode_t mode_of(const std::string& path) {
    struct stat st{};
    assert(stat(path.c_str(), &st) == 0);
    return st.st_mode & 0777;
}

int main() {
    // With no umask, a file created with a wider mode than 0600 would show it.
    umask(0);

    const std::string base = (std::filesystem::temp_directory_path() /
        ("kvstore-file-modes-" + std::to_string(getpid()))).string();
    const std::string wal_path = base + ".wal";
    const std::string snapshot_path = base + ".snapshot";
    std::remove(wal_path.c_str());
    std::remove(snapshot_path.c_str());

    {
        WAL wal(wal_path);
        wal.write_ahead(OPCODE_SET, "k", "v");
    }
    assert(mode_of(wal_path) == 0600);

    KVStore store;
    store.set("k", "v");
    Snapshot snapshot;
    snapshot.write(store, snapshot_path);
    assert(mode_of(snapshot_path) == 0600);
    assert(!std::filesystem::exists(snapshot_path + ".tmp"));

    // A stale temporary snapshot is removed; a missing one is not an error.
    {
        std::ofstream stale(snapshot_path + ".tmp");
        stale << "unfinished checkpoint";
    }
    Snapshot::discard_stale_temp(snapshot_path);
    assert(!std::filesystem::exists(snapshot_path + ".tmp"));
    Snapshot::discard_stale_temp(snapshot_path);

    // The published snapshot is untouched by the cleanup.
    KVStore reloaded;
    snapshot.recover(reloaded, snapshot_path);
    assert(reloaded.get("k") == std::optional<std::string>{"v"});

    std::remove(wal_path.c_str());
    std::remove(snapshot_path.c_str());
    return 0;
}
