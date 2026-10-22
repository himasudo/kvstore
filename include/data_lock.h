#pragma once

#include <string>

// Exclusive advisory lock on a file in the data directory, held for the life of
// the object. Two server processes sharing one WAL and snapshot would corrupt
// each other's files, and SO_REUSEPORT lets both of them bind the listen port,
// so the lock is what keeps a second instance out.
//
// The kernel drops the lock when the process exits for any reason, including
// SIGKILL, so a crash never leaves a stale lock behind.
class DataDirLock {
    public:
        // Throws std::runtime_error with a readable message if another process
        // (or another DataDirLock) already holds the lock, and
        // std::system_error for other failures.
        explicit DataDirLock(const std::string& path);
        ~DataDirLock();

        DataDirLock(const DataDirLock&) = delete;
        DataDirLock& operator=(const DataDirLock&) = delete;

    private:
        int fd_ = -1;
};
