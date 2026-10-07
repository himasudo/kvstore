#include "wal.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <cerrno>
#include <limits>
#include <system_error>

WAL::WAL() : fd_(-1) {}

WAL::WAL(const std::string& path) {
    fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd_ == -1) {
        throw std::runtime_error("Failed to open or create WAL file.");
    }
}

WAL::~WAL() {
    if (fd_ != -1) {
        fsync(fd_);
        close(fd_);
    }
}

void WAL::write_ahead(uint8_t opcode, const std::string& key, const std::string& value) {
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

    if (key.size() > std::numeric_limits<uint32_t>::max() ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("WAL key or value is too large");
    }

    uint32_t key_len = static_cast<uint32_t>(key.size());
    uint32_t val_len = static_cast<uint32_t>(value.size());
    uint64_t payload_size = sizeof(opcode) + sizeof(key_len) + key_len + sizeof(val_len) + val_len;

    if (payload_size > WAL_MAX_PAYLOAD_SIZE) {
        throw std::length_error("WAL record exceeds maximum size");
    }

    uint32_t total_size = static_cast<uint32_t>(payload_size);
    uint32_t checksum = 0;
    std::vector<uint8_t> command_buf;
    command_buf.reserve(total_size + WAL_HEADER_SIZE);
    
    auto append_u32 = [&command_buf](uint32_t val) {
        uint8_t* bytes = reinterpret_cast<uint8_t*>(&val);
        for (int i = 0; i < 4; i++) {
            command_buf.push_back(bytes[i]);
        }
    };

    append_u32(total_size);
    append_u32(checksum);
    command_buf.push_back(opcode);
    append_u32(key_len);
    command_buf.insert(command_buf.end(), key.begin(), key.end());
    append_u32(val_len);
    command_buf.insert(command_buf.end(), value.begin(), value.end());

    size_t sent = 0;
    uint8_t* buf_data = command_buf.data();
    size_t buf_size = command_buf.size();
    while(sent < buf_size) {
        ssize_t w = write(fd_, buf_data + sent, buf_size - sent);
        if (w == -1) {
            if (errno == EINTR) {
                continue; 
            }
            throw std::system_error(errno, std::generic_category(), "WAL write failed");
        }
        sent += static_cast<size_t>(w);
    }

    while (fsync(fd_) == -1) {
        if (errno == EINTR) {
            continue;
        }
        throw std::system_error(errno, std::generic_category(), "WAL fsync failed");
    }
}

std::vector<Command> WAL::recover() {
    std::vector<Command> commands;

    struct stat st{};
    if (fstat(fd_, &st) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to stat WAL");
    }

    if (lseek(fd_, 0, SEEK_SET) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to seek to beginning of WAL");
    }

    auto read_exact = [this](void* buf, size_t count) {
        size_t bytes_read = 0;
        char* ptr = static_cast<char*>(buf);

        while (bytes_read < count) {
            ssize_t r = read(fd_, ptr + bytes_read, count - bytes_read);
            if (r == 0) {
                throw std::runtime_error("Corrupted WAL: unexpected EOF");
            }
            if (r == -1) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "WAL read failed");
            }
            bytes_read += static_cast<size_t>(r);
        }
    };

    auto truncate_torn_tail = [this](off_t offset) {
        if (ftruncate(fd_, offset) == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to truncate torn WAL tail");
        }
        while (fsync(fd_) == -1) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "Failed to sync repaired WAL");
        }
    };

    off_t offset = 0;
    const off_t file_size = st.st_size;

    while (offset < file_size) {
        off_t remaining = file_size - offset;
        if (remaining < static_cast<off_t>(WAL_HEADER_SIZE)) {
            truncate_torn_tail(offset);
            break;
        }

        uint32_t header[2];
        read_exact(header, sizeof(header));
        uint32_t total_size = header[0];
        [[maybe_unused]] uint32_t checksum = header[1];

        if (total_size < WAL_MIN_PAYLOAD_SIZE || total_size > WAL_MAX_PAYLOAD_SIZE) {
            throw std::runtime_error("Corrupted WAL: invalid record size");
        }

        if (static_cast<uint64_t>(remaining - WAL_HEADER_SIZE) < total_size) {
            truncate_torn_tail(offset);
            break;
        }

        std::vector<uint8_t> payload(total_size);
        read_exact(payload.data(), payload.size());

        size_t payload_offset = 0;
        auto require = [&](size_t count) {
            if (count > payload.size() - payload_offset) {
                throw std::runtime_error("Corrupted WAL: field exceeds record boundary");
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

        uint32_t key_len = read_u32();
        require(key_len);
        std::string key(reinterpret_cast<char*>(payload.data() + payload_offset), key_len);
        payload_offset += key_len;

        uint32_t val_len = read_u32();
        require(val_len);
        std::string value(reinterpret_cast<char*>(payload.data() + payload_offset), val_len);
        payload_offset += val_len;

        if (payload_offset != payload.size()) {
            throw std::runtime_error("Corrupted WAL: record has trailing bytes");
        }

        Command cmd;
        if (opcode == OPCODE_SET) {
            cmd.type = Command::Type::SET;
            cmd.args = {std::move(key), std::move(value)};
        } 
        else if (opcode == OPCODE_DEL) {
            if (!value.empty()) {
                throw std::runtime_error("Corrupted WAL: DEL record contains a value");
            }
            cmd.type = Command::Type::DEL;
            cmd.args = {std::move(key)};
        }
        else if (opcode == OPCODE_CLEAR) {
            if (!key.empty() || !value.empty()) {
                throw std::runtime_error("Corrupted WAL: CLEAR record contains arguments");
            }
            cmd.type = Command::Type::CLEAR;
        }
        else {
            throw std::runtime_error("Corrupted WAL: unknown opcode");
        }

        commands.push_back(std::move(cmd));
        offset += static_cast<off_t>(WAL_HEADER_SIZE + total_size);
    }

    if (lseek(fd_, 0, SEEK_END) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to seek to end of WAL after recovery");
    }

    return commands;
}

void WAL::reset() {
    if (fd_ == -1) return;
    if (ftruncate(fd_, 0) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to truncate WAL");
    }
    
    if (lseek(fd_, 0, SEEK_SET) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to seek to beginning of WAL after truncate");
    }
}
