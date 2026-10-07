#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "wal.h"

static void append_bytes(const std::string& path, const void* data, size_t size) {
    int fd = open(path.c_str(), O_WRONLY | O_APPEND);
    assert(fd != -1);
    const auto* bytes = static_cast<const char*>(data);
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, bytes + written, size - written);
        assert(n > 0);
        written += static_cast<size_t>(n);
    }
    close(fd);
}

static off_t file_size(const std::string& path) {
    struct stat st{};
    assert(stat(path.c_str(), &st) == 0);
    return st.st_size;
}

static void expect_corruption(const std::string& path) {
    bool threw = false;
    try {
        WAL wal(path);
        (void)wal.recover();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

int main() {
    const auto base = std::filesystem::temp_directory_path() /
        ("kvstore-wal-recovery-" + std::to_string(getpid()));

    const std::string invalid_write_path = base.string() + "-invalid-write.wal";
    std::remove(invalid_write_path.c_str());
    {
        WAL wal(invalid_write_path);
        bool bad_opcode = false;
        try {
            wal.write_ahead(0xff, "key", "value");
        } catch (const std::invalid_argument&) {
            bad_opcode = true;
        }
        assert(bad_opcode);

        bool bad_del = false;
        try {
            wal.write_ahead(OPCODE_DEL, "key", "unexpected-value");
        } catch (const std::invalid_argument&) {
            bad_del = true;
        }
        assert(bad_del);
    }
    std::remove(invalid_write_path.c_str());

    const std::string torn_path = base.string() + "-torn.wal";
    std::remove(torn_path.c_str());
    off_t valid_size = 0;
    {
        WAL wal(torn_path);
        wal.write_ahead(OPCODE_SET, "stable", "value");
    }
    valid_size = file_size(torn_path);

    const uint8_t partial_record[] = {0x20, 0x00, 0x00, 0x00, 0x00};
    append_bytes(torn_path, partial_record, sizeof(partial_record));
    assert(file_size(torn_path) > valid_size);
    {
        WAL wal(torn_path);
        auto commands = wal.recover();
        assert(commands.size() == 1);
        assert(commands[0].type == Command::Type::SET);
        assert(commands[0].args[0] == "stable");
        assert(commands[0].args[1] == "value");
    }
    assert(file_size(torn_path) == valid_size);
    std::remove(torn_path.c_str());

    const std::string oversized_path = base.string() + "-oversized.wal";
    std::remove(oversized_path.c_str());
    {
        int fd = open(oversized_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        uint32_t header[2] = {WAL_MAX_PAYLOAD_SIZE + 1, 0};
        assert(write(fd, header, sizeof(header)) == static_cast<ssize_t>(sizeof(header)));
        close(fd);
    }
    expect_corruption(oversized_path);
    std::remove(oversized_path.c_str());

    const std::string bad_length_path = base.string() + "-bad-length.wal";
    std::remove(bad_length_path.c_str());
    {
        int fd = open(bad_length_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        uint32_t header[2] = {WAL_MIN_PAYLOAD_SIZE, 0};
        uint8_t payload[WAL_MIN_PAYLOAD_SIZE]{};
        payload[0] = OPCODE_SET;
        uint32_t impossible_key_len = 100;
        std::memcpy(payload + 1, &impossible_key_len, sizeof(impossible_key_len));
        assert(write(fd, header, sizeof(header)) == static_cast<ssize_t>(sizeof(header)));
        assert(write(fd, payload, sizeof(payload)) == static_cast<ssize_t>(sizeof(payload)));
        close(fd);
    }
    expect_corruption(bad_length_path);
    std::remove(bad_length_path.c_str());

    const std::string bad_opcode_path = base.string() + "-bad-opcode.wal";
    std::remove(bad_opcode_path.c_str());
    {
        int fd = open(bad_opcode_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        uint32_t header[2] = {WAL_MIN_PAYLOAD_SIZE, 0};
        uint8_t payload[WAL_MIN_PAYLOAD_SIZE]{};
        payload[0] = 0xff;
        assert(write(fd, header, sizeof(header)) == static_cast<ssize_t>(sizeof(header)));
        assert(write(fd, payload, sizeof(payload)) == static_cast<ssize_t>(sizeof(payload)));
        close(fd);
    }
    expect_corruption(bad_opcode_path);
    std::remove(bad_opcode_path.c_str());

    return 0;
}
