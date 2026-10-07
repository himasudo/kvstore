#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "kvstore.h"
#include "snapshot.h"

static void write_all(int fd, const void* data, size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, bytes + written, size - written);
        assert(n > 0);
        written += static_cast<size_t>(n);
    }
}

static void write_record(int fd, uint8_t opcode, const std::string& key, const std::string& value) {
    uint32_t key_len = static_cast<uint32_t>(key.size());
    uint32_t val_len = static_cast<uint32_t>(value.size());
    uint32_t total_size = 1 + 4 + key_len + 4 + val_len;
    uint32_t header[2] = {total_size, 0};

    write_all(fd, header, sizeof(header));
    write_all(fd, &opcode, sizeof(opcode));
    write_all(fd, &key_len, sizeof(key_len));
    if (!key.empty()) write_all(fd, key.data(), key.size());
    write_all(fd, &val_len, sizeof(val_len));
    if (!value.empty()) write_all(fd, value.data(), value.size());
}

static void expect_corruption_without_mutation(const std::string& path) {
    KVStore target;
    target.set("sentinel", "keep");

    bool threw = false;
    try {
        Snapshot snapshot;
        snapshot.recover(target, path);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    assert(threw);
    assert(target.size() == 1);
    assert(target.get("sentinel") == std::optional<std::string>{"keep"});
}

int main() {
    const auto base = std::filesystem::temp_directory_path() /
        ("kvstore-snapshot-recovery-" + std::to_string(getpid()));

    const std::string valid_path = base.string() + "-valid.snapshot";
    std::remove(valid_path.c_str());
    {
        KVStore source;
        source.set("alpha", "one");
        source.set("beta", "two");
        Snapshot snapshot;
        snapshot.write(source, valid_path);
    }
    {
        KVStore recovered;
        Snapshot snapshot;
        snapshot.recover(recovered, valid_path);
        assert(recovered.size() == 2);
        assert(recovered.get("alpha") == std::optional<std::string>{"one"});
        assert(recovered.get("beta") == std::optional<std::string>{"two"});
    }
    std::remove(valid_path.c_str());

    const std::string partial_path = base.string() + "-partial.snapshot";
    std::remove(partial_path.c_str());
    {
        int fd = open(partial_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        write_record(fd, SNAPSHOT_OPCODE_SET, "would-leak", "value");
        const uint8_t partial_header[] = {0x20, 0x00, 0x00};
        write_all(fd, partial_header, sizeof(partial_header));
        close(fd);
    }
    expect_corruption_without_mutation(partial_path);
    std::remove(partial_path.c_str());

    const std::string truncated_payload_path = base.string() + "-truncated-payload.snapshot";
    std::remove(truncated_payload_path.c_str());
    {
        int fd = open(truncated_payload_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        uint32_t header[2] = {SNAPSHOT_MIN_PAYLOAD_SIZE + 20, 0};
        write_all(fd, header, sizeof(header));
        uint8_t partial_payload[SNAPSHOT_MIN_PAYLOAD_SIZE]{};
        partial_payload[0] = SNAPSHOT_OPCODE_SET;
        write_all(fd, partial_payload, sizeof(partial_payload));
        close(fd);
    }
    expect_corruption_without_mutation(truncated_payload_path);
    std::remove(truncated_payload_path.c_str());

    const std::string oversized_path = base.string() + "-oversized.snapshot";
    std::remove(oversized_path.c_str());
    {
        int fd = open(oversized_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        uint32_t header[2] = {SNAPSHOT_MAX_PAYLOAD_SIZE + 1, 0};
        write_all(fd, header, sizeof(header));
        close(fd);
    }
    expect_corruption_without_mutation(oversized_path);
    std::remove(oversized_path.c_str());

    const std::string bad_length_path = base.string() + "-bad-length.snapshot";
    std::remove(bad_length_path.c_str());
    {
        int fd = open(bad_length_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        uint32_t header[2] = {SNAPSHOT_MIN_PAYLOAD_SIZE, 0};
        uint8_t payload[SNAPSHOT_MIN_PAYLOAD_SIZE]{};
        payload[0] = SNAPSHOT_OPCODE_SET;
        uint32_t impossible_key_len = 100;
        std::memcpy(payload + 1, &impossible_key_len, sizeof(impossible_key_len));
        write_all(fd, header, sizeof(header));
        write_all(fd, payload, sizeof(payload));
        close(fd);
    }
    expect_corruption_without_mutation(bad_length_path);
    std::remove(bad_length_path.c_str());

    const std::string bad_opcode_path = base.string() + "-bad-opcode.snapshot";
    std::remove(bad_opcode_path.c_str());
    {
        int fd = open(bad_opcode_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        write_record(fd, 0xff, "key", "value");
        close(fd);
    }
    expect_corruption_without_mutation(bad_opcode_path);
    std::remove(bad_opcode_path.c_str());

    return 0;
}
