#include "snapshot.h"
#include <fcntl.h>
#include <unistd.h>
#include <system_error>
#include <cerrno>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>

void Snapshot::write(const KVStore& store, const std::string& path) {
    std::vector<std::pair<std::string, std::string>> entries = store.entries();
    std::string tmp_path = path + ".tmp";
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to open temporary snapshot file");
    }

    std::vector<uint8_t> command_buf;
    for (const auto& [key, value] : entries) {
        command_buf.clear();
        uint8_t opcode = 0; // 0 = SET command
        uint32_t key_len = key.size();
        uint32_t val_len = value.size();

        uint32_t total_size = sizeof(opcode) + sizeof(key_len) + key_len + sizeof(val_len) + val_len;
        uint32_t checksum = 0; 

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
            ssize_t w = ::write(fd, buf_data + sent, buf_size - sent);
            if (w == -1) {
                if (errno == EINTR) continue;
                int saved_errno = errno;
                close(fd); // Prevent fd leak on throw
                throw std::system_error(saved_errno, std::generic_category(), "Snapshot write failed");
            }
            sent += w;
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

    auto read_exact = [&fd](void* buf, size_t count) -> bool {
        size_t bytes_read = 0;
        char* ptr = static_cast<char*>(buf);
        
        while (bytes_read < count) {
            ssize_t r = ::read(fd, ptr + bytes_read, count - bytes_read);
            
            if (r == 0) {
                if (bytes_read == 0) return false;
                throw std::runtime_error("Corrupted Snapshot: unexpected EOF mid-entry");
            }
            if (r == -1) {
                if (errno == EINTR) continue; 
                throw std::system_error(errno, std::generic_category(), "Snapshot read failed");
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
        // checksum = header[1]; // Unused right now

        std::vector<uint8_t> payload(total_size);
        if (!read_exact(payload.data(), total_size)) {
            throw std::runtime_error("Corrupted Snapshot: failed to read full payload");
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

        if (opcode == 0) {
            store.set(key, value);
        } else {
            throw std::runtime_error("Corrupted Snapshot: encountered non-SET opcode");
        }
    }
}