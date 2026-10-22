#include "data_lock.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

DataDirLock::DataDirLock(const std::string& path) {
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(),
                                "Failed to open lock file " + path);
    }

    while (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EINTR) continue;
        int err = errno;
        close(fd);
        if (err == EWOULDBLOCK) {
            throw std::runtime_error(
                "another kvstore instance is already using this data directory (" +
                path + " is locked)");
        }
        throw std::system_error(err, std::generic_category(),
                                "Failed to lock " + path);
    }
    fd_ = fd;
}

DataDirLock::~DataDirLock() {
    if (fd_ != -1) close(fd_);  // closing the descriptor releases the lock
}
