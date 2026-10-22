#pragma once

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

#include "io_fault.h"

// fsync the directory that holds `path`, so a file created or renamed there
// survives a crash. Syncing the file's own data does not cover its directory
// entry on every filesystem. `fault_point` names the injectable failure used
// by the I/O fault tests.
inline void fsync_parent_directory(const std::string& path,
                                   const char* error_message,
                                   const char* fault_point) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";

    int dir_fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd == -1) {
        throw std::system_error(errno, std::generic_category(), error_message);
    }

    int err = 0;
    while (persistence_fsync(dir_fd, fault_point) == -1) {
        if (errno == EINTR) continue;
        err = errno;
        break;
    }
    close(dir_fd);
    if (err != 0) {
        throw std::system_error(err, std::generic_category(), error_message);
    }
}
