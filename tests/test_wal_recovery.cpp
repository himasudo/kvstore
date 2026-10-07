#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "persistence_format.h"
#include "wal.h"

static constexpr std::array<uint8_t, 4> WAL_MAGIC{'K', 'V', 'W', 'L'};

static void write_all(int fd, const void* data, size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, bytes + written, size - written);
        assert(n > 0);
        written += static_cast<size_t>(n);
    }
}

static void create_wal_file(const std::string& path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd != -1);
    auto header = make_file_header(WAL_MAGIC);
    write_all(fd, header.data(), header.size());
    close(fd);
}

static void append_bytes(const std::string& path, const void* data, size_t size) {
    int fd = open(path.c_str(), O_WRONLY | O_APPEND);
    assert(fd != -1);
    write_all(fd, data, size);
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

static void append_record(const std::string& path, uint8_t opcode,
                          const std::string& key, const std::string& value) {
    auto record = encode_record(opcode, key, value);
    append_bytes(path, record.data(), record.size());
}

int main() {
    const auto base = std::filesystem::temp_directory_path() /
        ("kvstore-wal-recovery-" + std::to_string(getpid()));

    const std::string invalid_write_path = base.string() + "-invalid-write.wal";
    std::remove(invalid_write_path.c_str());
    {
        WAL wal(invalid_write_path);
        bool bad_opcode = false;
        try { wal.write_ahead(0xff, "key", "value"); }
        catch (const std::invalid_argument&) { bad_opcode = true; }
        assert(bad_opcode);

        bool bad_del = false;
        try { wal.write_ahead(OPCODE_DEL, "key", "unexpected-value"); }
        catch (const std::invalid_argument&) { bad_del = true; }
        assert(bad_del);
    }
    std::remove(invalid_write_path.c_str());

    const std::string torn_header_path = base.string() + "-torn-header.wal";
    std::remove(torn_header_path.c_str());
    {
        int fd = open(torn_header_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        const uint8_t partial[] = {'K', 'V', 'W'};
        write_all(fd, partial, sizeof(partial));
        close(fd);
    }
    {
        WAL wal(torn_header_path);
        assert(wal.recover().empty());
    }
    assert(file_size(torn_header_path) == static_cast<off_t>(PERSISTENCE_FILE_HEADER_SIZE));
    std::remove(torn_header_path.c_str());

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
    {
        WAL wal(torn_path);
        auto commands = wal.recover();
        assert(commands.size() == 1);
        assert(commands[0].args[0] == "stable");
        assert(commands[0].args[1] == "value");
    }
    assert(file_size(torn_path) == valid_size);
    std::remove(torn_path.c_str());

    const std::string bad_checksum_path = base.string() + "-bad-checksum.wal";
    std::remove(bad_checksum_path.c_str());
    {
        WAL wal(bad_checksum_path);
        wal.write_ahead(OPCODE_SET, "key", "value");
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
    expect_corruption(bad_checksum_path);
    std::remove(bad_checksum_path.c_str());

    const std::string oversized_path = base.string() + "-oversized.wal";
    std::remove(oversized_path.c_str());
    create_wal_file(oversized_path);
    {
        uint8_t header[PERSISTENCE_RECORD_HEADER_SIZE]{};
        uint32_t size = PERSISTENCE_MAX_PAYLOAD_SIZE + 1;
        header[0] = static_cast<uint8_t>(size);
        header[1] = static_cast<uint8_t>(size >> 8);
        header[2] = static_cast<uint8_t>(size >> 16);
        header[3] = static_cast<uint8_t>(size >> 24);
        append_bytes(oversized_path, header, sizeof(header));
    }
    expect_corruption(oversized_path);
    std::remove(oversized_path.c_str());

    const std::string bad_length_path = base.string() + "-bad-length.wal";
    std::remove(bad_length_path.c_str());
    create_wal_file(bad_length_path);
    {
        std::vector<uint8_t> payload(PERSISTENCE_MIN_PAYLOAD_SIZE, 0);
        payload[0] = OPCODE_SET;
        uint32_t impossible_key_len = 100;
        payload[1] = static_cast<uint8_t>(impossible_key_len);
        payload[2] = static_cast<uint8_t>(impossible_key_len >> 8);
        payload[3] = static_cast<uint8_t>(impossible_key_len >> 16);
        payload[4] = static_cast<uint8_t>(impossible_key_len >> 24);
        uint32_t size = static_cast<uint32_t>(payload.size());
        uint32_t checksum = crc32c(payload);
        uint8_t header[PERSISTENCE_RECORD_HEADER_SIZE] = {
            static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
            static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 24),
            static_cast<uint8_t>(checksum), static_cast<uint8_t>(checksum >> 8),
            static_cast<uint8_t>(checksum >> 16), static_cast<uint8_t>(checksum >> 24)
        };
        append_bytes(bad_length_path, header, sizeof(header));
        append_bytes(bad_length_path, payload.data(), payload.size());
    }
    expect_corruption(bad_length_path);
    std::remove(bad_length_path.c_str());

    const std::string bad_opcode_path = base.string() + "-bad-opcode.wal";
    std::remove(bad_opcode_path.c_str());
    create_wal_file(bad_opcode_path);
    append_record(bad_opcode_path, 0xff, "", "");
    expect_corruption(bad_opcode_path);
    std::remove(bad_opcode_path.c_str());

    const std::string bad_magic_path = base.string() + "-bad-magic.wal";
    std::remove(bad_magic_path.c_str());
    {
        int fd = open(bad_magic_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        const uint8_t bad_header[PERSISTENCE_FILE_HEADER_SIZE] = {'B','A','D','!',1,0,0,0};
        write_all(fd, bad_header, sizeof(bad_header));
        close(fd);
    }
    expect_corruption(bad_magic_path);
    std::remove(bad_magic_path.c_str());

    const std::string bad_version_path = base.string() + "-bad-version.wal";
    std::remove(bad_version_path.c_str());
    {
        int fd = open(bad_version_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd != -1);
        const uint8_t bad_header[PERSISTENCE_FILE_HEADER_SIZE] = {'K','V','W','L',2,0,0,0};
        write_all(fd, bad_header, sizeof(bad_header));
        close(fd);
    }
    expect_corruption(bad_version_path);
    std::remove(bad_version_path.c_str());

    const std::string reset_path = base.string() + "-reset.wal";
    std::remove(reset_path.c_str());
    {
        WAL wal(reset_path);
        wal.write_ahead(OPCODE_SET, "key", "value");
        wal.reset();
        assert(wal.recover().empty());
    }
    assert(file_size(reset_path) == static_cast<off_t>(PERSISTENCE_FILE_HEADER_SIZE));
    std::remove(reset_path.c_str());

    return 0;
}
