#include "snapshot.h"
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <system_error>
#include <cerrno>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <filesystem>

static constexpr std::array<uint8_t, 4> SNAPSHOT_MAGIC{'K', 'V', 'S', 'S'};

static void write_all(int fd, const uint8_t* data, size_t size, const char* error_message) {
    size_t written = 0;
    while (written < size) {
        ssize_t result = write(fd, data + written, size - written);
        if (result == -1) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), error_message);
        }
        if (result == 0) {
            throw std::runtime_error(std::string(error_message) + ": zero-byte write");
        }
        written += static_cast<size_t>(result);
    }
}

static void read_exact(int fd, void* buf, size_t count, const char* error_message) {
    size_t bytes_read = 0;
    char* ptr = static_cast<char*>(buf);
    while (bytes_read < count) {
        ssize_t result = read(fd, ptr + bytes_read, count - bytes_read);
        if (result == 0) {
            throw std::runtime_error("Corrupted Snapshot: unexpected EOF");
        }
        if (result == -1) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), error_message);
        }
        bytes_read += static_cast<size_t>(result);
    }
}

static void sync_fd(int fd, const char* error_message) {
    while (fsync(fd) == -1) {
        if (errno == EINTR) continue;
        throw std::system_error(errno, std::generic_category(), error_message);
    }
}

static void fsync_parent_directory(const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        parent = ".";
    }

    int dir_fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to open snapshot directory");
    }

    try {
        sync_fd(dir_fd, "Snapshot directory fsync failed");
    } catch (...) {
        close(dir_fd);
        throw;
    }
    close(dir_fd);
}

void Snapshot::write(const KVStore& store, const std::string& path) {
    std::vector<std::pair<std::string, std::string>> entries = store.entries();
    std::string tmp_path = path + ".tmp";
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to open temporary snapshot file");
    }

    try {
        auto file_header = make_file_header(SNAPSHOT_MAGIC);
        write_all(fd, file_header.data(), file_header.size(), "Snapshot header write failed");

        for (const auto& [key, value] : entries) {
            std::vector<uint8_t> record = encode_record(SNAPSHOT_OPCODE_SET, key, value);
            write_all(fd, record.data(), record.size(), "Snapshot write failed");
        }

        sync_fd(fd, "Snapshot fsync failed");
    } catch (...) {
        close(fd);
        throw;
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
    if (st.st_size < static_cast<off_t>(PERSISTENCE_FILE_HEADER_SIZE)) {
        throw std::runtime_error("Corrupted Snapshot: truncated file header");
    }

    std::array<uint8_t, PERSISTENCE_FILE_HEADER_SIZE> file_header{};
    read_exact(fd, file_header.data(), file_header.size(), "Snapshot header read failed");
    validate_file_header(file_header, SNAPSHOT_MAGIC);

    std::vector<std::pair<std::string, std::string>> recovered_entries;
    off_t offset = static_cast<off_t>(PERSISTENCE_FILE_HEADER_SIZE);
    const off_t file_size = st.st_size;

    while (offset < file_size) {
        off_t remaining = file_size - offset;
        if (remaining < static_cast<off_t>(PERSISTENCE_RECORD_HEADER_SIZE)) {
            throw std::runtime_error("Corrupted Snapshot: truncated record header");
        }

        std::array<uint8_t, PERSISTENCE_RECORD_HEADER_SIZE> raw_header{};
        read_exact(fd, raw_header.data(), raw_header.size(), "Snapshot record header read failed");
        PersistenceRecordHeader header = decode_record_header(raw_header);

        if (static_cast<uint64_t>(remaining - PERSISTENCE_RECORD_HEADER_SIZE) < header.payload_size) {
            throw std::runtime_error("Corrupted Snapshot: truncated record payload");
        }

        std::vector<uint8_t> payload(header.payload_size);
        read_exact(fd, payload.data(), payload.size(), "Snapshot payload read failed");
        PersistenceRecord record = decode_record_payload(payload, header.checksum);

        if (record.opcode != SNAPSHOT_OPCODE_SET) {
            throw std::runtime_error("Corrupted Snapshot: encountered non-SET opcode");
        }

        recovered_entries.emplace_back(std::move(record.key), std::move(record.value));
        offset += static_cast<off_t>(PERSISTENCE_RECORD_HEADER_SIZE + header.payload_size);
    }

    for (auto& [key, value] : recovered_entries) {
        store.set(std::move(key), std::move(value));
    }
}
