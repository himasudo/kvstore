#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "kvstore.h"
#include "persistence_format.h"
#include "snapshot.h"

static constexpr std::array<uint8_t, 4> SNAPSHOT_MAGIC{'K', 'V', 'S', 'S'};

static void write_all(int fd, const void* data, size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, bytes + written, size - written);
        assert(n > 0);
        written += static_cast<size_t>(n);
    }
}

static void create_snapshot_file(const std::string& path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd != -1);
    auto header = make_file_header(SNAPSHOT_MAGIC);
    write_all(fd, header.data(), header.size());
    close(fd);
}

static void append_bytes(const std::string& path, const void* data, size_t size) {
    int fd = open(path.c_str(), O_WRONLY | O_APPEND);
    assert(fd != -1);
    write_all(fd, data, size);
    close(fd);
}

static void append_record(const std::string& path, uint8_t opcode,
                          const std::string& key, const std::string& value) {
    auto record = encode_record(opcode, key, value);
    append_bytes(path, record.data(), record.size());
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

    const std::string empty_path = base.string() + "-empty.snapshot";
    std::remove(empty_path.c_str());
    {
        KVStore source;
        Snapshot snapshot;
        snapshot.write(source, empty_path);
        KVStore recovered;
        snapshot.recover(recovered, empty_path);
        assert(recovered.size() == 0);
    }
    std::remove(empty_path.c_str());

    const std::string partial_file_header_path = base.string() + "-partial-file-header.snapshot";
    std::remove(partial_file_header_path.c_str());
    {
        int fd = open(partial_file_header_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        const uint8_t partial[] = {'K', 'V', 'S'};
        write_all(fd, partial, sizeof(partial));
        close(fd);
    }
    expect_corruption_without_mutation(partial_file_header_path);
    std::remove(partial_file_header_path.c_str());

    const std::string partial_record_path = base.string() + "-partial-record.snapshot";
    std::remove(partial_record_path.c_str());
    create_snapshot_file(partial_record_path);
    append_record(partial_record_path, SNAPSHOT_OPCODE_SET, "would-leak", "value");
    {
        const uint8_t partial[] = {0x20, 0x00, 0x00};
        append_bytes(partial_record_path, partial, sizeof(partial));
    }
    expect_corruption_without_mutation(partial_record_path);
    std::remove(partial_record_path.c_str());

    const std::string truncated_payload_path = base.string() + "-truncated-payload.snapshot";
    std::remove(truncated_payload_path.c_str());
    create_snapshot_file(truncated_payload_path);
    {
        uint8_t header[PERSISTENCE_RECORD_HEADER_SIZE]{};
        uint32_t size = PERSISTENCE_MIN_PAYLOAD_SIZE + 20;
        header[0] = static_cast<uint8_t>(size);
        header[1] = static_cast<uint8_t>(size >> 8);
        header[2] = static_cast<uint8_t>(size >> 16);
        header[3] = static_cast<uint8_t>(size >> 24);
        append_bytes(truncated_payload_path, header, sizeof(header));
        uint8_t partial[PERSISTENCE_MIN_PAYLOAD_SIZE]{};
        append_bytes(truncated_payload_path, partial, sizeof(partial));
    }
    expect_corruption_without_mutation(truncated_payload_path);
    std::remove(truncated_payload_path.c_str());

    const std::string bad_checksum_path = base.string() + "-bad-checksum.snapshot";
    std::remove(bad_checksum_path.c_str());
    {
        KVStore source;
        source.set("key", "value");
        Snapshot snapshot;
        snapshot.write(source, bad_checksum_path);
    }
    {
        int fd = open(bad_checksum_path.c_str(), O_RDWR);
        assert(fd != -1);
        off_t pos = static_cast<off_t>(PERSISTENCE_FILE_HEADER_SIZE + PERSISTENCE_RECORD_HEADER_SIZE + 6);
        assert(lseek(fd, pos, SEEK_SET) == pos);
        uint8_t byte;
        assert(read(fd, &byte, 1) == 1);
        byte ^= 0x01;
        assert(lseek(fd, pos, SEEK_SET) == pos);
        assert(write(fd, &byte, 1) == 1);
        close(fd);
    }
    expect_corruption_without_mutation(bad_checksum_path);
    std::remove(bad_checksum_path.c_str());

    const std::string oversized_path = base.string() + "-oversized.snapshot";
    std::remove(oversized_path.c_str());
    create_snapshot_file(oversized_path);
    {
        uint8_t header[PERSISTENCE_RECORD_HEADER_SIZE]{};
        uint32_t size = PERSISTENCE_MAX_PAYLOAD_SIZE + 1;
        header[0] = static_cast<uint8_t>(size);
        header[1] = static_cast<uint8_t>(size >> 8);
        header[2] = static_cast<uint8_t>(size >> 16);
        header[3] = static_cast<uint8_t>(size >> 24);
        append_bytes(oversized_path, header, sizeof(header));
    }
    expect_corruption_without_mutation(oversized_path);
    std::remove(oversized_path.c_str());

    const std::string bad_opcode_path = base.string() + "-bad-opcode.snapshot";
    std::remove(bad_opcode_path.c_str());
    create_snapshot_file(bad_opcode_path);
    append_record(bad_opcode_path, 0xff, "key", "value");
    expect_corruption_without_mutation(bad_opcode_path);
    std::remove(bad_opcode_path.c_str());

    const std::string bad_magic_path = base.string() + "-bad-magic.snapshot";
    std::remove(bad_magic_path.c_str());
    {
        int fd = open(bad_magic_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        const uint8_t bad_header[PERSISTENCE_FILE_HEADER_SIZE] = {'B','A','D','!',1,0,0,0};
        write_all(fd, bad_header, sizeof(bad_header));
        close(fd);
    }
    expect_corruption_without_mutation(bad_magic_path);
    std::remove(bad_magic_path.c_str());

    const std::string bad_version_path = base.string() + "-bad-version.snapshot";
    std::remove(bad_version_path.c_str());
    {
        int fd = open(bad_version_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        const uint8_t bad_header[PERSISTENCE_FILE_HEADER_SIZE] = {'K','V','S','S',2,0,0,0};
        write_all(fd, bad_header, sizeof(bad_header));
        close(fd);
    }
    expect_corruption_without_mutation(bad_version_path);
    std::remove(bad_version_path.c_str());

    return 0;
}
