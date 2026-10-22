#include <cassert>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "data_lock.h"

int main() {
    const std::string path = (std::filesystem::temp_directory_path() /
        ("kvstore-lock-" + std::to_string(getpid()) + ".lock")).string();
    std::remove(path.c_str());

    auto first = std::make_unique<DataDirLock>(path);

    struct stat st{};
    assert(stat(path.c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    // A second holder is refused while the first is alive.
    bool refused = false;
    try {
        DataDirLock second(path);
    } catch (const std::runtime_error& e) {
        refused = std::string(e.what()).find("already using") != std::string::npos;
    }
    assert(refused);

    // Releasing the first lock lets another holder in.
    first.reset();
    { DataDirLock again(path); }

    // The lock file is left in place; its presence alone means nothing.
    assert(stat(path.c_str(), &st) == 0);
    std::remove(path.c_str());

    bool bad_path = false;
    try {
        DataDirLock nowhere("/nonexistent-directory/kvstore.lock");
    } catch (const std::exception&) {
        bad_path = true;
    }
    assert(bad_path);
    return 0;
}
