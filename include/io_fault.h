#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#ifdef KVSTORE_ENABLE_IO_FAULTS

#include <atomic>
#include <cstdlib>
#include <string>

// Configure KVSTORE_IO_FAULT before starting I/O threads; one matching call
// consumes the fault process-wide. Do not mutate the environment during I/O.
inline bool take_io_fault(const char* point, const char* suffix) {
    static std::atomic<bool> consumed{false};
    if (consumed.load(std::memory_order_relaxed)) return false;

    const char* active = std::getenv("KVSTORE_IO_FAULT");
    if (active == nullptr) return false;

    const size_t point_len = std::strlen(point);
    const size_t suffix_len = std::strlen(suffix);
    if (std::strlen(active) != point_len + suffix_len) return false;
    if (std::memcmp(active, point, point_len) != 0) return false;
    if (std::memcmp(active + point_len, suffix, suffix_len) != 0) return false;

    return !consumed.exchange(true, std::memory_order_relaxed);
}

inline ssize_t persistence_write(int fd, const void* buf, size_t count, const char* point) {
    if (take_io_fault(point, "_eintr")) {
        errno = EINTR;
        return -1;
    }
    if (take_io_fault(point, "_enospc")) {
        errno = ENOSPC;
        return -1;
    }
    if (take_io_fault(point, "_eio")) {
        errno = EIO;
        return -1;
    }
    // The retry belongs to the writing thread and this specific I/O target.
    static thread_local int partial_eio_fd = -1;
    static thread_local std::string partial_eio_point;
    if (partial_eio_fd == fd && partial_eio_point == point) {
        partial_eio_fd = -1;
        errno = EIO;
        return -1;
    }
    if (count > 1 && take_io_fault(point, "_partial_eio")) {
        ssize_t result = ::write(fd, buf, count / 2);
        if (result > 0) {
            partial_eio_point = point;
            partial_eio_fd = fd;
        }
        return result;
    }
    if (take_io_fault(point, "_short") && count > 1) {
        return ::write(fd, buf, count / 2);
    }
    return ::write(fd, buf, count);
}

inline int persistence_fsync(int fd, const char* point) {
    if (take_io_fault(point, "_eintr")) {
        errno = EINTR;
        return -1;
    }
    if (take_io_fault(point, "_eio")) {
        errno = EIO;
        return -1;
    }
    return ::fsync(fd);
}

inline int persistence_ftruncate(int fd, off_t length, const char* point) {
    if (take_io_fault(point, "_eio")) {
        errno = EIO;
        return -1;
    }
    return ::ftruncate(fd, length);
}

inline int persistence_rename(const char* old_path, const char* new_path, const char* point) {
    if (take_io_fault(point, "_eio")) {
        errno = EIO;
        return -1;
    }
    return std::rename(old_path, new_path);
}

#else

inline ssize_t persistence_write(int fd, const void* buf, size_t count, const char*) {
    return ::write(fd, buf, count);
}

inline int persistence_fsync(int fd, const char*) {
    return ::fsync(fd);
}

inline int persistence_ftruncate(int fd, off_t length, const char*) {
    return ::ftruncate(fd, length);
}

inline int persistence_rename(const char* old_path, const char* new_path, const char*) {
    return std::rename(old_path, new_path);
}

#endif
