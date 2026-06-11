//
// Created by Brian Song on 5/18/26.
//

#ifndef ORDERBEAMER_FD_H
#define ORDERBEAMER_FD_H
#include <unistd.h>


class FD {
    int fd;
public:
    explicit FD(const int fd) noexcept : fd{fd} {}
    FD() noexcept : fd{-1} {}
    [[nodiscard]] bool fdValid() const noexcept {
        return fd != -1;
    }
    operator int() const {
        return fd;
    }
    FD(const FD& other) = delete;
    FD& operator=(const FD&) = delete;
    FD(FD&& other) noexcept : fd{other.fd} {
        other.fd = -1;
    }
    FD& operator=(FD&& other) noexcept {
        if (this != &other) {
            if (fd != -1) close(fd);
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    ~FD() noexcept {
        if (fd != -1) {
            close(fd);
        }
    }
};


#endif //ORDERBEAMER_FD_H
