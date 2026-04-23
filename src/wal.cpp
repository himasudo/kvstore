#include "wal.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <cerrno>
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

    uint32_t key_len = key.size();
    uint32_t val_len = value.size();

    uint32_t total_size = sizeof(opcode) + sizeof(key_len) + key_len + sizeof(val_len) + val_len;
    uint32_t checksum = 0;
    std::vector<uint8_t> command_buf;
    command_buf.reserve(total_size + 8);
    
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
        int w = write(fd_, buf_data + sent, buf_size - sent);
    if (w == -1) {
            if (errno == EINTR) {
                continue; 
            }
            throw std::system_error(errno, std::generic_category(), "WAL write failed");
        }
        sent += w;
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

    if (lseek(fd_, 0, SEEK_SET) == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to seek to beginning of WAL");
    }

    auto read_exact = [this](void* buf, size_t count) -> bool {
        size_t bytes_read = 0;
        char* ptr = static_cast<char*>(buf);
        while (bytes_read < count) {
            ssize_t r = read(fd_, ptr + bytes_read, count - bytes_read);
            
            if (r == 0) {
                if (bytes_read == 0) return false; 
                throw std::runtime_error("Corrupted WAL: unexpected EOF mid-entry");
            }
            if (r == -1) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "WAL read failed");
            }
            bytes_read += r;
        }
        return true;
    };

    while (true) {
        uint32_t header[2];
        
        if (!read_exact(header, sizeof(header))) {
            break; 
        }

        uint32_t total_size = header[0];
        [[maybe_unused]] uint32_t checksum = header[1];

        std::vector<uint8_t> payload(total_size);
        if (!read_exact(payload.data(), total_size)) {
            throw std::runtime_error("Corrupted WAL: failed to read full payload");
        }

        size_t offset = 0;

        uint8_t opcode = payload[offset];
        offset += sizeof(opcode);

        uint32_t key_len;
        std::memcpy(&key_len, payload.data() + offset, sizeof(key_len));
        offset += sizeof(key_len);

        std::string key(reinterpret_cast<char*>(payload.data() + offset), key_len);
        offset += key_len;


        uint32_t val_len;
        std::memcpy(&val_len, payload.data() + offset, sizeof(val_len));
        offset += sizeof(val_len);

        std::string value(reinterpret_cast<char*>(payload.data() + offset), val_len);

        if (opcode == OPCODE_SET) {
            Command cmd;
            cmd.type = Command::Type::SET;
            cmd.args = {key, value};
            commands.push_back(cmd);
        } 
        else if (opcode == OPCODE_DEL) {
            Command cmd;
            cmd.type = Command::Type::DEL;
            cmd.args = {key};
            commands.push_back(cmd);
        }
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