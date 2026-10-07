#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <csignal>
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

static auto sorted_entries(KVStore& store) {
    auto entries = store.entries();
    std::sort(entries.begin(), entries.end());
    return entries;
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

static void expect_sigkill(pid_t pid) {
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGKILL);
}

static void clean_files(const std::string& snapshot_path, const std::string& wal_path) {
    std::remove(snapshot_path.c_str());
    std::remove((snapshot_path + ".tmp").c_str());
    std::remove(wal_path.c_str());
}

static void seed_checkpoint_state(const std::string& wal_path) {
    KVStore store;
    WAL wal(wal_path);
    Dispatcher dispatcher(store, wal);

    assert(dispatcher.dispatch(Command{Command::Type::SET, {"a", "1"}}));
    assert(dispatcher.dispatch(Command{Command::Type::SET, {"b", "old"}}));
    assert(dispatcher.dispatch(Command{Command::Type::CLEAR, {}}));
    assert(dispatcher.dispatch(Command{Command::Type::SET, {"b", "2"}}));
    assert(dispatcher.dispatch(Command{Command::Type::SET, {"c", "3"}}));
    assert(dispatcher.dispatch(Command{Command::Type::DEL, {"c"}}));
    assert(dispatcher.dispatch(Command{Command::Type::SET, {"d", "4"}}));
}

static void run_checkpoint_failpoint(const std::string& failpoint,
                                     const std::string& snapshot_path,
                                     const std::string& wal_path) {
    clean_files(snapshot_path, wal_path);
    seed_checkpoint_state(wal_path);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        KVStore store;
        WAL wal(wal_path);
        replay(store, wal.recover());

        Dispatcher dispatcher(store, wal);
        Snapshot snapshot;

        assert(setenv("KVSTORE_FAILPOINT", failpoint.c_str(), 1) == 0);
        dispatcher.checkpoint(snapshot, snapshot_path);
        _exit(2);
    }

    expect_sigkill(pid);

    const std::vector<std::pair<std::string, std::string>> expected{
        {"b", "2"},
        {"d", "4"},
    };
    assert(recover_state(snapshot_path, wal_path) == expected);

    clean_files(snapshot_path, wal_path);
}

static void run_mutation_failpoint(const std::string& failpoint,
                                   const std::string& snapshot_path,
                                   const std::string& wal_path) {
    clean_files(snapshot_path, wal_path);

    {
        KVStore store;
        WAL wal(wal_path);
        Dispatcher dispatcher(store, wal);
        assert(dispatcher.dispatch(Command{Command::Type::SET, {"shared", "old"}}));
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        KVStore store;
        WAL wal(wal_path);
        replay(store, wal.recover());

        Dispatcher dispatcher(store, wal);
        assert(setenv("KVSTORE_FAILPOINT", failpoint.c_str(), 1) == 0);
        (void)dispatcher.dispatch(Command{Command::Type::SET, {"shared", "new"}});
        _exit(2);
    }

    expect_sigkill(pid);

    KVStore recovered;
    WAL wal(wal_path);
    replay(recovered, wal.recover());
    assert(recovered.get("shared") == std::optional<std::string>{"new"});

    clean_files(snapshot_path, wal_path);
}

int main() {
    const auto base = std::filesystem::temp_directory_path() /
        ("kvstore-failpoints-" + std::to_string(getpid()));

    const std::vector<std::string> mutation_failpoints{
        "wal_after_write",
        "wal_after_fsync",
    };

    for (size_t i = 0; i < mutation_failpoints.size(); ++i) {
        run_mutation_failpoint(
            mutation_failpoints[i],
            base.string() + "-mutation-" + std::to_string(i) + ".snapshot",
            base.string() + "-mutation-" + std::to_string(i) + ".wal");
    }

    const std::vector<std::string> checkpoint_failpoints{
        "snapshot_after_file_fsync",
        "snapshot_after_rename",
        "snapshot_after_dir_fsync",
        "wal_reset_after_truncate",
        "wal_reset_after_header_write",
    };

    for (size_t i = 0; i < checkpoint_failpoints.size(); ++i) {
        run_checkpoint_failpoint(
            checkpoint_failpoints[i],
            base.string() + "-checkpoint-" + std::to_string(i) + ".snapshot",
            base.string() + "-checkpoint-" + std::to_string(i) + ".wal");
    }

    return 0;
}
