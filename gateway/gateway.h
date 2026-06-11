//
// Created by Brian Song on 5/17/26.
//

#ifndef ORDERBEAMER_GATEWAY_H
#define ORDERBEAMER_GATEWAY_H
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "engine/statics.h"
#include "../memory/Buffer.h"
#include "generics/Error.h"
#include "generics/FD.h"

template <typename T, int N> // N is the capacity of the buffer
class Gateway {
    // Listening socket
    FD listenFd{-1};
    // epoll socket
    FD epollFd{-1};
    std::unordered_map<int, FD> newConnectionFds;
    sockaddr_un sadr{};
    Buffer<T, N>& buf;
    Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE>& errorBuf;
    bool errorsFoundDuringSetup {false};
public:
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Gateway(const std::string& socketPath, const uint16_t port, Buffer<T, N>& buffer, Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE>& errorBuf) noexcept : buf{buffer}, errorBuf {errorBuf} {
        listenFd = FD{socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0)};
        if (!listenFd.fdValid()) [[unlikely]] {
            Error::generate(errorBuf, "GATEWAY: Listener could not be created", errno);
            errorsFoundDuringSetup = true;
            return;
        }
        epollFd = FD{epoll_create1(0)};
        if (!epollFd.fdValid()) [[unlikely]] {
            Error::generate(errorBuf, "GATEWAY: Epoll could not be created", errno);
            errorsFoundDuringSetup = true;
            return;
        }
        epoll_event ev{};
        constexpr int opt{1};
        if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) [[unlikely]] {
            Error::generate(errorBuf, "GATEWAY: Socket reuse options could not be initialized", errno);
            errorsFoundDuringSetup = true;
            return;
        }
        sadr.sun_family = AF_UNIX;
        std::strncpy(sadr.sun_path, socketPath.c_str(), sizeof(sadr.sun_path) - 1);
        // Clear any stale socket file left by a previous run
        unlink(sadr.sun_path);

        if (bind(listenFd, reinterpret_cast<sockaddr *>(&sadr), sizeof(sadr)) == -1) [[unlikely]] {
            Error::generate(errorBuf, "GATEWAY: FD could not be bound to socket address.", errno);
            errorsFoundDuringSetup = true;
            return;
        }
        if (listen(listenFd, SOMAXCONN) == -1) [[unlikely]] {
            Error::generate(errorBuf, "GATEWAY: Socket could not be marked as listening socket", errno);
            errorsFoundDuringSetup = true;
            return;
        }

        ev.events = EPOLLIN;
        ev.data.fd = listenFd;
        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, listenFd, &ev) == -1) [[unlikely]] {
            Error::generate(errorBuf, "GATEWAY: epoll could not be created", errno);
            errorsFoundDuringSetup = true;
            return;
        }
    }

    void run() noexcept {
        // Spins in a while loop.
        if (errorsFoundDuringSetup || !epollFd.fdValid() || !listenFd.fdValid()) [[unlikely]] {
            Error::generate(errorBuf, "GATEWAY RUN: Epoll and Socket could not be created", errno);
            return;
        }

        constexpr int MAX_EVENTS {gateway_constants::MAX_EPOLL_EVENTS};
        epoll_event events[MAX_EVENTS];

        while (true) {
            const int numFds = epoll_wait(epollFd, events, MAX_EVENTS, -1);

            if (numFds == -1) {
                if (errno == EINTR) {
                    continue;
                }
                Error::generate(errorBuf, "GATEWAY RUN: epoll_wait failed", errno);
                return;
            }

            for (int i {0}; i < numFds; ++i) {
                if (events[i].data.fd == listenFd) {
                    // Handle new listen connection(s) by looping through
                    while (true) {
                        int fd {accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK)};
                        if (fd == -1) [[unlikely]] {
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            Error::generate(errorBuf, "GATEWAY RUN: an instance of accept failed", errno);
                        }
                        newConnectionFds[fd] = FD {fd};

                        // Add to epoll watchlist
                        epoll_event newConnEvent {};
                        newConnEvent.events = EPOLLIN | EPOLLET;
                        newConnEvent.data.fd = fd;
                        epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &newConnEvent);
                    }
                } else {
                    // Read from FD and push to SPSC (heap-shared memory with processing thread)
                    while (true) {
                        Order newOrder {};
                        const ssize_t amountRead {read(events[i].data.fd, &newOrder, sizeof(Order))};
                        if (amountRead == 0) {
                            // Reached EOF on socket (socket closed). Remove and continue loop
                            epoll_ctl(epollFd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
                            newConnectionFds.erase(events[i].data.fd);
                            break;
                        }
                        if (amountRead == -1 || amountRead != sizeof(Order)) {
                            if (amountRead == -1) [[unlikely]] {
                                if (errno == EINTR) {
                                    continue;
                                }
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    break;
                                }
                            }
                            Error::generate(errorBuf, "GATEWAY RUN: read failed", errno);
                            break;
                        }

                        // At this point, newOrder is a valid Order struct
                        // Busy-spin Buffer to allow it to empty
                        while (!buf.tryWrite(newOrder)) {
                            intrinsic_funcs::efficientPause();
                        }
                    }
                }
            }
        }
    }
};

#endif //ORDERBEAMER_GATEWAY_H
