#pragma once

#ifdef KVSTORE_ENABLE_FAILPOINTS

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

inline void crash_failpoint(const char* name) {
    const char* active = std::getenv("KVSTORE_FAILPOINT");
    if (active != nullptr && std::strcmp(active, name) == 0) {
        (void)kill(getpid(), SIGKILL);
    }
}

#else

inline void crash_failpoint(const char*) {}

#endif
