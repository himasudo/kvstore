#include <algorithm>
#include <cassert>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "dispatcher.h"
#include "io_fault.h"
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

template <typename F>
static void expect_rejected(F operation) {
    bool threw = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

static void verify_failed_wal(Dispatcher& dispatcher, WAL& wal, KVStore& store,
                              const std::string& snapshot_path,
                              const std::string& wal_path) {
    const auto before = sorted_entries(store);
    const auto wal_size = std::filesystem::file_size(wal_path);
    const bool snapshot_exists = std::filesystem::exists(snapshot_path);
    std::vector<std::thread> writers;
    for (const auto& command : std::vector<Command>{
             {Command::Type::SET, {"later", "value"}},
             {Command::Type::DEL, {"shared"}},
             {Command::Type::CLEAR, {}}}) {
        writers.emplace_back([&, command] {
            expect_rejected([&] { (void)dispatcher.dispatch(command); });
        });
    }
    for (auto& writer : writers) writer.join();
    Snapshot snapshot;
    expect_rejected([&] { dispatcher.checkpoint(snapshot, snapshot_path); });
    expect_rejected([&] { wal.reset(); });
    expect_rejected([&] { (void)wal.recover(); });
    expect_rejected([&] { wal.write_ahead(OPCODE_SET, "direct", "value"); });
    assert(sorted_entries(store) == before);
    assert(std::filesystem::file_size(wal_path) == wal_size);
    assert(std::filesystem::exists(snapshot_path) == snapshot_exists);
    assert(dispatcher.dispatch(Command{Command::Type::GET, {"shared"}}));
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
        if (fault == "wal_record_write_partial_eio") {
            const auto expected_size = PERSISTENCE_FILE_HEADER_SIZE +
                encode_record(OPCODE_SET, "shared", "old").size() +
                encode_record(OPCODE_SET, "shared", "new").size() / 2;
            assert(std::filesystem::file_size(wal_path) == expected_size);
        }
        verify_failed_wal(dispatcher, wal, store, wal_path + ".snapshot", wal_path);
        _exit(0);
    }

    wait_ok(pid);
    {
        WAL wal(wal_path);
        KVStore recovered;
        replay(recovered, wal.recover());
        assert(recovered.get("shared").has_value());
        assert(!recovered.exists("later"));
        if (fault != "wal_record_fsync_eio") {
            assert(recovered.get("shared") == std::optional<std::string>{"old"});
        }
        Dispatcher dispatcher(recovered, wal);
        assert(dispatcher.dispatch(Command{Command::Type::SET, {"restarted", "ok"}}));
    }
    {
        WAL wal(wal_path);
        KVStore recovered;
        replay(recovered, wal.recover());
        assert(recovered.get("restarted") == std::optional<std::string>{"ok"});
    }
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
        if (fault.starts_with("wal_")) {
            verify_failed_wal(dispatcher, wal, store, snapshot_path, wal_path);
        }
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

// Each child retains the injector's process-wide, one-shot lifetime.
static void run_concurrent_injection(bool partial) {
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        assert(setenv("KVSTORE_IO_FAULT",
                      partial ? "target_partial_eio" : "target_eio", 1) == 0);
        FILE* first = std::tmpfile();
        FILE* other = std::tmpfile();
        assert(first && other);
        const int fd = fileno(first);
        const int other_fd = fileno(other);
        constexpr int threads = 8;
        std::barrier start(threads);
        std::barrier after_write(threads);
        std::atomic<int> injected{0};
        std::vector<std::thread> writers;
        for (int i = 0; i < threads; ++i) {
            writers.emplace_back([&] {
                start.arrive_and_wait();
                const ssize_t result = persistence_write(fd, "abcd", 4, "target");
                const int error = errno;
                after_write.arrive_and_wait();
                if (partial && result == 2) {
                    ++injected;
                    // Neither a different descriptor nor a different point may
                    // consume the error owed to this thread's retry.
                    assert(persistence_write(other_fd, "abcd", 4, "target") == 4);
                    assert(persistence_write(fd, "abcd", 4, "other") == 4);
                    assert(persistence_write(fd, "cd", 2, "target") == -1);
                    assert(errno == EIO);
                    assert(persistence_write(fd, "cd", 2, "target") == 2);
                } else if (!partial && result == -1) {
                    assert(error == EIO);
                    ++injected;
                } else {
                    assert(result == 4);
                }
            });
        }
        for (auto& writer : writers) writer.join();
        assert(injected == 1);
        std::fclose(first);
        std::fclose(other);
        _exit(0);
    }
    wait_ok(pid);
}

int main() {
    run_concurrent_injection(false);
    run_concurrent_injection(true);
    const auto base = std::filesystem::temp_directory_path() /
        ("kvstore-io-faults-" + std::to_string(getpid()));

    run_transient_wal_fault(
        "wal_record_write_short", base.string() + "-short.wal");
    run_transient_wal_fault(
        "wal_record_write_eintr", base.string() + "-write-eintr.wal");
    run_transient_wal_fault(
        "wal_record_fsync_eintr", base.string() + "-fsync-eintr.wal");

    run_failed_mutation(
        "wal_record_write_partial_eio", base.string() + "-partial-eio.wal");
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
        "wal_header_write_eio",
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
