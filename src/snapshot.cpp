#include "snapshot.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <system_error>
#include <cerrno>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <limits>

static void fsync_parent_directory(const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        parent = ".";
    }

    int dir_fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to open snapshot directory");
    }

    while (fsync(dir_fd) == -1) {
        if (errno == EINTR) continue;
        int saved_errno = errno;
        close(dir_fd);
        throw std::system_error(saved_errno, std::generic_category(), "Snapshot directory fsync failed");
    }

    close(dir_fd);
}

void Snapshot::write(const KVStore& store, const std::string& path) {
    std::vector<std::pair<std::string, std::string>> entries = store.entries();

    for (const auto& [key, value] : entries) {
        if (key.size() > std::numeric_limits<uint32_t>::max() ||
            value.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::length_error("Snapshot key or value is too large");
        }

        uint64_t payload_size = sizeof(uint8_t) + sizeof(uint32_t) + key.size() +
                                sizeof(uint32_t) + value.size();
        if (payload_size > SNAPSHOT_MAX_PAYLOAD_SIZE) {
            throw std::length_error("Snapshot record exceeds maximum size");
        }
    }

    std::string tmp_path = path + ".tmp";
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to open temporary snapshot file");
    }

    std::vector<uint8_t> command_buf;
    for (const auto& [key, value] : entries) {
        command_buf.clear();
        uint32_t key_len = static_cast<uint32_t>(key.size());
        uint32_t val_len = static_cast<uint32_t>(value.size());
        uint32_t total_size = static_cast<uint32_t>(
            sizeof(SNAPSHOT_OPCODE_SET) + sizeof(key_len) + key_len + sizeof(val_len) + val_len);
        uint32_t checksum = 0;

        command_buf.reserve(total_size + SNAPSHOT_HEADER_SIZE);

        auto append_u32 = [&command_buf](uint32_t val) {
            uint8_t* bytes = reinterpret_cast<uint8_t*>(&val);
            for (int i = 0; i < 4; i++) {
                command_buf.push_back(bytes[i]);
            }
        };

        append_u32(total_size);
        append_u32(checksum);
        command_buf.push_back(SNAPSHOT_OPCODE_SET);
        append_u32(key_len);
        command_buf.insert(command_buf.end(), key.begin(), key.end());
        append_u32(val_len);
        command_buf.insert(command_buf.end(), value.begin(), value.end());

        size_t sent = 0;
        while(sent < command_buf.size()) {
            ssize_t w = ::write(fd, command_buf.data() + sent, command_buf.size() - sent);
            if (w == -1) {
                if (errno == EINTR) continue;
                int saved_errno = errno;
                close(fd);
                throw std::system_error(saved_errno, std::generic_category(), "Snapshot write failed");
            }
            sent += static_cast<size_t>(w);
        }
    }

    while (fsync(fd) == -1) {
        if (errno == EINTR) continue;
        int saved_errno = errno;
        close(fd);
        throw std::system_error(saved_errno, std::generic_category(), "Snapshot fsync failed");
    }

    close(fd);

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to rename snapshot tmp file");
    }

    fsync_parent_directory(path);
}

void Snapshot::recover(KVStore& store, const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT) {
            return;
        }
        throw std::system_error(errno, std::generic_category(), "Failed to open snapshot file");
    }

    struct ScopedFD {
        int fd;
        ~ScopedFD() { if (fd != -1) close(fd); }
    } scoped_fd{fd};

    struct stat st{};
    if (fstat(fd, &st) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to stat snapshot file");
    }

    auto read_exact = [&fd](void* buf, size_t count) {
        size_t bytes_read = 0;
        char* ptr = static_cast<char*>(buf);

        while (bytes_read < count) {
            ssize_t r = ::read(fd, ptr + bytes_read, count - bytes_read);
            if (r == 0) {
                throw std::runtime_error("Corrupted Snapshot: unexpected EOF");
            }
            if (r == -1) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "Snapshot read failed");
            }
            bytes_read += static_cast<size_t>(r);
        }
    };

    std::vector<std::pair<std::string, std::string>> recovered_entries;
    off_t offset = 0;
    const off_t file_size = st.st_size;

    while (offset < file_size) {
        off_t remaining = file_size - offset;
        if (remaining < static_cast<off_t>(SNAPSHOT_HEADER_SIZE)) {
            throw std::runtime_error("Corrupted Snapshot: truncated record header");
        }

        uint32_t header[2];
        read_exact(header, sizeof(header));
        uint32_t total_size = header[0];
        [[maybe_unused]] uint32_t checksum = header[1];

        if (total_size < SNAPSHOT_MIN_PAYLOAD_SIZE || total_size > SNAPSHOT_MAX_PAYLOAD_SIZE) {
            throw std::runtime_error("Corrupted Snapshot: invalid record size");
        }

        if (static_cast<uint64_t>(remaining - SNAPSHOT_HEADER_SIZE) < total_size) {
            throw std::runtime_error("Corrupted Snapshot: truncated record payload");
        }

        std::vector<uint8_t> payload(total_size);
        read_exact(payload.data(), payload.size());

        size_t payload_offset = 0;
        auto require = [&](size_t count) {
            if (count > payload.size() - payload_offset) {
                throw std::runtime_error("Corrupted Snapshot: field exceeds record boundary");
            }
        };
        auto read_u32 = [&]() {
            require(sizeof(uint32_t));
            uint32_t value;
            std::memcpy(&value, payload.data() + payload_offset, sizeof(value));
            payload_offset += sizeof(value);
            return value;
        };

        require(sizeof(uint8_t));
        uint8_t opcode = payload[payload_offset++];
        if (opcode != SNAPSHOT_OPCODE_SET) {
            throw std::runtime_error("Corrupted Snapshot: encountered non-SET opcode");
        }

        uint32_t key_len = read_u32();
        require(key_len);
        std::string key(reinterpret_cast<char*>(payload.data() + payload_offset), key_len);
        payload_offset += key_len;

        uint32_t val_len = read_u32();
        require(val_len);
        std::string value(reinterpret_cast<char*>(payload.data() + payload_offset), val_len);
        payload_offset += val_len;

        if (payload_offset != payload.size()) {
            throw std::runtime_error("Corrupted Snapshot: record has trailing bytes");
        }

        recovered_entries.emplace_back(std::move(key), std::move(value));
        offset += static_cast<off_t>(SNAPSHOT_HEADER_SIZE + total_size);
    }

    for (auto& [key, value] : recovered_entries) {
        store.set(std::move(key), std::move(value));
    }
}
