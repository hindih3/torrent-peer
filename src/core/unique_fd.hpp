#pragma once
#include <cerrno>
#include <unistd.h>

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&)            = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& o) noexcept : fd_(o.release()) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        reset(o.release());
        return *this;
    }

    [[nodiscard]] int  get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    // allows directly calling if (sock) instead of if (sock.valid())
    explicit operator bool() const noexcept { return valid(); }

    // Take ownership back from the wrapper; caller must close it.
    [[nodiscard]] int release() noexcept {
        int f = fd_;
        fd_ = -1;
        return f;
    }

    // Close the current fd (if any) and adopt `f`. Leaves errno untouched.
    void reset(int f = -1) noexcept {
        if (fd_ >= 0 && fd_ != f) {
            const int saved = errno;
            close(fd_);
            errno = saved;
        }
        fd_ = f;
    }

private:
    int fd_ = -1;
};