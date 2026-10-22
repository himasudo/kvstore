#include "wal.h"
#include "failpoint.h"
#include "io_fault.h"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>
#include <vector>
#include <cerrno>
#include <system_error>

static constexpr std::array<uint8_t, 4> WAL_MAGIC{'K', 'V', 'W', 'L'};

static void write_all(int fd, const uint8_t* data, size_t size,
                      const char* error_message, const char* fault_point) {
    size_t written = 0;
    while (written < size) {
        ssize_t result = persistence_write(
            fd, data + written, size - written, fault_point);
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
            throw std::runtime_error("Corrupted WAL: unexpected EOF");
        }
        if (result == -1) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), error_message);
        }
        bytes_read += static_cast<size_t>(result);
    }
}

static void sync_fd(int fd, const char* error_message, const char* fault_point) {
    while (persistence_fsync(fd, fault_point) == -1) {
        if (errno == EINTR) continue;
        throw std::system_error(errno, std::generic_category(), error_message);
    }
}

static void write_wal_header(int fd) {
    auto header = make_file_header(WAL_MAGIC);
    write_all(fd, header.data(), header.size(),
              "WAL header write failed", "wal_header_write");
}

WAL::WAL() : fd_(-1) {}

WAL::WAL(const std::string& path) : fd_(-1) {
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        throw std::runtime_error("Failed to open or create WAL file.");
    }

    try {
        struct stat st{};
        if (fstat(fd, &st) == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to stat WAL");
        }

        if (st.st_size < static_cast<off_t>(PERSISTENCE_FILE_HEADER_SIZE)) {
            if (persistence_ftruncate(fd, 0, "wal_header_truncate") == -1) {
                throw std::system_error(errno, std::generic_category(), "Failed to repair torn WAL header");
            }
            write_wal_header(fd);
            sync_fd(fd, "WAL header fsync failed", "wal_header_fsync");
        } else {
            if (lseek(fd, 0, SEEK_SET) == -1) {
                throw std::system_error(errno, std::generic_category(), "Failed to seek to WAL header");
            }
            std::array<uint8_t, PERSISTENCE_FILE_HEADER_SIZE> header{};
            read_exact(fd, header.data(), header.size(), "WAL header read failed");
            validate_file_header(header, WAL_MAGIC);
        }

        if (lseek(fd, 0, SEEK_END) == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to seek to end of WAL");
        }
        fd_ = fd;
    } catch (...) {
        close(fd);
        throw;
    }
}

WAL::~WAL() {
    if (fd_ != -1) {
        fsync(fd_);
        close(fd_);
    }
}

void WAL::ensure_writable() const {
    if (failed_) {
        throw std::runtime_error("WAL persistence failed; restart and recovery required");
    }
}

void WAL::write_ahead(uint8_t opcode, const std::string& key, const std::string& value) {
    ensure_writable();
    if (fd_ == -1) return;

    if (opcode != OPCODE_SET && opcode != OPCODE_DEL && opcode != OPCODE_CLEAR) {
        throw std::invalid_argument("Unknown WAL opcode");
    }
    if (opcode == OPCODE_DEL && !value.empty()) {
        throw std::invalid_argument("DEL WAL record cannot contain a value");
    }
    if (opcode == OPCODE_CLEAR && (!key.empty() || !value.empty())) {
        throw std::invalid_argument("CLEAR WAL record cannot contain arguments");
    }

    std::vector<uint8_t> record = encode_record(opcode, key, value);
    try {
        write_all(fd_, record.data(), record.size(),
                  "WAL write failed", "wal_record_write");
        crash_failpoint("wal_after_write");
        sync_fd(fd_, "WAL fsync failed", "wal_record_fsync");
        crash_failpoint("wal_after_fsync");
    } catch (...) {
        failed_ = true;
        throw;
    }
}

std::vector<Command> WAL::recover() try {
    ensure_writable();
    std::vector<Command> commands;

    struct stat st{};
    if (fstat(fd_, &st) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to stat WAL");
    }

    if (lseek(fd_, PERSISTENCE_FILE_HEADER_SIZE, SEEK_SET) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to seek to first WAL record");
    }

    const off_t file_size = st.st_size;

    auto truncate_torn_tail = [this, file_size](off_t offset) {
        if (persistence_ftruncate(fd_, offset, "wal_repair_truncate") == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to truncate torn WAL tail");
        }
        sync_fd(fd_, "Failed to sync repaired WAL", "wal_repair_fsync");
        truncated_tail_bytes_ += static_cast<uint64_t>(file_size - offset);
    };

    // True when every byte from `from` to EOF is zero. A crash can extend the
    // file before the data reaches disk, which leaves exactly this shape.
    auto rest_is_zero = [this, file_size](off_t from) {
        std::array<uint8_t, 64 * 1024> buf;
        off_t pos = from;
        while (pos < file_size) {
            size_t want = static_cast<size_t>(
                std::min<off_t>(file_size - pos, static_cast<off_t>(buf.size())));
            ssize_t n = pread(fd_, buf.data(), want, pos);
            if (n == -1) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "WAL tail scan failed");
            }
            if (n == 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[static_cast<size_t>(i)] != 0) return false;
            }
            pos += n;
        }
        return true;
    };

    auto corrupted = [](const std::string& what, off_t offset) {
        return std::runtime_error("Corrupted WAL: " + what + " at offset " +
                                  std::to_string(offset));
    };

    off_t offset = static_cast<off_t>(PERSISTENCE_FILE_HEADER_SIZE);

    // A record that fails validation is a torn tail, not corruption, when it is
    // the last thing in the file (or only zero bytes follow it): a crash during
    // the final, never-acknowledged write can leave it in any of those shapes.
    // The same failure with more data after it means damage to a record that
    // may have been acknowledged, so recovery refuses to start.
    while (offset < file_size) {
        off_t remaining = file_size - offset;
        if (remaining < static_cast<off_t>(PERSISTENCE_RECORD_HEADER_SIZE)) {
            truncate_torn_tail(offset);
            break;
        }

        std::array<uint8_t, PERSISTENCE_RECORD_HEADER_SIZE> raw_header{};
        read_exact(fd_, raw_header.data(), raw_header.size(), "WAL record header read failed");
        PersistenceRecordHeader header{};
        try {
            header = decode_record_header(raw_header);
        } catch (const std::runtime_error&) {
            if (rest_is_zero(offset)) {
                truncate_torn_tail(offset);
                break;
            }
            throw corrupted("invalid record header", offset);
        }

        const uint64_t body_available =
            static_cast<uint64_t>(remaining - PERSISTENCE_RECORD_HEADER_SIZE);
        if (body_available < header.payload_size) {
            truncate_torn_tail(offset);
            break;
        }

        std::vector<uint8_t> payload(header.payload_size);
        read_exact(fd_, payload.data(), payload.size(), "WAL payload read failed");

        if (crc32c(payload) != header.checksum) {
            const off_t record_end = offset +
                static_cast<off_t>(PERSISTENCE_RECORD_HEADER_SIZE + header.payload_size);
            if (record_end == file_size || rest_is_zero(record_end)) {
                truncate_torn_tail(offset);
                break;
            }
            throw corrupted("checksum mismatch in a record followed by more data", offset);
        }

        PersistenceRecord record = decode_record_payload(payload, header.checksum);

        Command cmd;
        if (record.opcode == OPCODE_SET) {
            cmd.type = Command::Type::SET;
            cmd.args = {std::move(record.key), std::move(record.value)};
        }
        else if (record.opcode == OPCODE_DEL) {
            if (!record.value.empty()) {
                throw std::runtime_error("Corrupted WAL: DEL record contains a value");
            }
            cmd.type = Command::Type::DEL;
            cmd.args = {std::move(record.key)};
        }
        else if (record.opcode == OPCODE_CLEAR) {
            if (!record.key.empty() || !record.value.empty()) {
                throw std::runtime_error("Corrupted WAL: CLEAR record contains arguments");
            }
            cmd.type = Command::Type::CLEAR;
        }
        else {
            throw std::runtime_error("Corrupted WAL: unknown opcode");
        }

        commands.push_back(std::move(cmd));
        offset += static_cast<off_t>(PERSISTENCE_RECORD_HEADER_SIZE + header.payload_size);
    }

    if (lseek(fd_, 0, SEEK_END) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to seek to end of WAL after recovery");
    }

    return commands;
} catch (...) {
    failed_ = true;
    throw;
}

void WAL::reset() {
    ensure_writable();
    if (fd_ == -1) return;

    try {
        if (persistence_ftruncate(fd_, 0, "wal_reset_truncate") == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to truncate WAL");
        }
        crash_failpoint("wal_reset_after_truncate");
        if (lseek(fd_, 0, SEEK_SET) == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to seek to beginning of WAL after truncate");
        }

        write_wal_header(fd_);
        crash_failpoint("wal_reset_after_header_write");
        sync_fd(fd_, "Failed to sync reset WAL", "wal_reset_fsync");

        if (lseek(fd_, 0, SEEK_END) == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to seek to end of reset WAL");
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}
