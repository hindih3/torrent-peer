#pragma once
#include <unistd.h>

struct UniqueFd {
    int fd = -1;

    UniqueFd() = default;
    explicit UniqueFd(const int f) : fd(f) {}
    ~UniqueFd() { if (fd >= 0) ::close(fd); }

    UniqueFd(const UniqueFd&)            = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) { if (fd >= 0) ::close(fd); fd = o.fd; o.fd = -1; }
        return *this;
    }
};
