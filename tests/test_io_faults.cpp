#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/wait.h>
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

static auto sorted_entries(KVStore& store) {
    auto entries = store.entries();
    std::sort(entries.begin(), entries.end());
    return entries;
}

static void wait_ok(pid_t pid) {
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static void cleanup(const std::string& snapshot_path, const std::string& wal_path) {
    std::remove(snapshot_path.c_str());
    std::remove((snapshot_path + ".tmp").c_str());
    std::remove(wal_path.c_str());
}

static std::vector<std::pair<std::string, std::string>> recover_state(
    const std::string& snapshot_path, const std::string& wal_path) {
    KVStore store;
    Snapshot snapshot;
    snapshot.recover(store, snapshot_path);

    WAL wal(wal_path);
    replay(store, wal.recover());
    return sorted_entries(store);
}

static void run_transient_wal_fault(const std::string& fault,
                                    const std::string& wal_path) {
    std::remove(wal_path.c_str());
    {
        WAL wal(wal_path);
        wal.write_ahead(OPCODE_SET, "base", "one");
    }

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        KVStore store;
        WAL wal(wal_path);
        replay(store, wal.recover());
        Dispatcher dispatcher(store, wal);

        assert(setenv("KVSTORE_IO_FAULT", fault.c_str(), 1) == 0);
        auto result = dispatcher.dispatch(
            Command{Command::Type::SET, {"after", "two"}});
        assert(result.has_value());
        assert(store.get("after") == std::optional<std::string>{"two"});
        _exit(0);
    }

    wait_ok(pid);

    KVStore recovered;
    {
        WAL wal(wal_path);
        replay(recovered, wal.recover());
    }
    assert(recovered.get("base") == std::optional<std::string>{"one"});
    assert(recovered.get("after") == std::optional<std::string>{"two"});
    std::remove(wal_path.c_str());
}

static void run_failed_mutation(const std::string& fault,
                                const std::string& wal_path) {
    std::remove(wal_path.c_str());
    {
        WAL wal(wal_path);
        wal.write_ahead(OPCODE_SET, "shared", "old");
    }

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        KVStore store;
        WAL wal(wal_path);
        replay(store, wal.recover());
        Dispatcher dispatcher(store, wal);

        assert(setenv("KVSTORE_IO_FAULT", fault.c_str(), 1) == 0);
        bool threw = false;
        try {
            (void)dispatcher.dispatch(
                Command{Command::Type::SET, {"shared", "new"}});
        } catch (const std::system_error&) {
            threw = true;
        }

        assert(threw);
        assert(store.get("shared") == std::optional<std::string>{"old"});
        _exit(0);
    }

    wait_ok(pid);
    std::remove(wal_path.c_str());
}

static void seed_checkpoint(const std::string& wal_path) {
    KVStore store;
    WAL wal(wal_path);
    Dispatcher dispatcher(store, wal);

    assert(dispatcher.dispatch(Command{Command::Type::SET, {"a", "1"}}));
    assert(dispatcher.dispatch(Command{Command::Type::SET, {"b", "2"}}));
    assert(dispatcher.dispatch(Command{Command::Type::DEL, {"a"}}));
    assert(dispatcher.dispatch(Command{Command::Type::SET, {"c", "3"}}));
}

static void run_checkpoint_fault(const std::string& fault,
                                 bool expect_success,
                                 const std::string& snapshot_path,
                                 const std::string& wal_path) {
    cleanup(snapshot_path, wal_path);
    seed_checkpoint(wal_path);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        KVStore store;
        WAL wal(wal_path);
        replay(store, wal.recover());
        Dispatcher dispatcher(store, wal);
        Snapshot snapshot;

        assert(setenv("KVSTORE_IO_FAULT", fault.c_str(), 1) == 0);

        bool threw = false;
        try {
            dispatcher.checkpoint(snapshot, snapshot_path);
        } catch (const std::system_error&) {
            threw = true;
        }

        assert(threw != expect_success);
        _exit(0);
    }

    wait_ok(pid);

    const std::vector<std::pair<std::string, std::string>> expected{
        {"b", "2"},
        {"c", "3"},
    };
    assert(recover_state(snapshot_path, wal_path) == expected);
    cleanup(snapshot_path, wal_path);
}

int main() {
    const auto base = std::filesystem::temp_directory_path() /
        ("kvstore-io-faults-" + std::to_string(getpid()));

    run_transient_wal_fault(
        "wal_record_write_short", base.string() + "-short.wal");
    run_transient_wal_fault(
        "wal_record_write_eintr", base.string() + "-write-eintr.wal");
    run_transient_wal_fault(
        "wal_record_fsync_eintr", base.string() + "-fsync-eintr.wal");

    run_failed_mutation(
        "wal_record_write_enospc", base.string() + "-enospc.wal");
    run_failed_mutation(
        "wal_record_fsync_eio", base.string() + "-fsync-eio.wal");

    const std::vector<std::string> transient_checkpoint_faults{
        "snapshot_write_short",
        "snapshot_write_eintr",
        "snapshot_file_fsync_eintr",
    };
    for (size_t i = 0; i < transient_checkpoint_faults.size(); ++i) {
        run_checkpoint_fault(
            transient_checkpoint_faults[i],
            true,
            base.string() + "-transient-" + std::to_string(i) + ".snapshot",
            base.string() + "-transient-" + std::to_string(i) + ".wal");
    }

    const std::vector<std::string> failed_checkpoint_faults{
        "snapshot_write_enospc",
        "snapshot_file_fsync_eio",
        "snapshot_rename_eio",
        "snapshot_dir_fsync_eio",
        "wal_reset_truncate_eio",
        "wal_reset_fsync_eio",
    };
    for (size_t i = 0; i < failed_checkpoint_faults.size(); ++i) {
        run_checkpoint_fault(
            failed_checkpoint_faults[i],
            false,
            base.string() + "-failed-" + std::to_string(i) + ".snapshot",
            base.string() + "-failed-" + std::to_string(i) + ".wal");
    }

    return 0;
}
