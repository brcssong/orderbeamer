//
// Created by Brian Song on 5/19/26.
//

#ifndef ORDERBEAMER_BUFFER_H
#define ORDERBEAMER_BUFFER_H

#include <atomic>
#include <concepts>
#include <cstddef>
#include <new>
#include <optional>
#include <utility>
#include "memory/Allocator.h"



template <std::copyable T, size_t N>
// Implements an atomic SPSC ring buffer.
class Buffer {
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> rPtr;
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> wPtr;
    T* data;

    static void increment(std::atomic<size_t>& ptr) noexcept {
        ptr.store(ptr.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    static size_t mask(const size_t ptr) noexcept {
        if constexpr (N & (N - 1)) {
            return ptr % N;
        } else {
            return ptr & (N - 1);
        }
    }
    [[nodiscard]] bool isEmpty() const noexcept {
        return wPtr.load(std::memory_order_acquire) == rPtr.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isFull() const noexcept {
        return wPtr.load(std::memory_order_relaxed) - rPtr.load(std::memory_order_acquire) == N;
    }
public:
    Buffer() noexcept : rPtr {0}, wPtr {0}, data {Allocator::allocate<T>(N)} {}
    Buffer(const Buffer&) = delete;
    Buffer(Buffer&&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer& operator=(Buffer&&) = delete;
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return N;
    }
    bool tryWrite(T elem) noexcept {
        if (isFull()) return false;
        data[mask(wPtr.load(std::memory_order_relaxed))] = std::move(elem);
        increment(wPtr);
        return true;
    }
    std::optional<T> tryRead() noexcept {
        const auto wp {wPtr.load(std::memory_order_acquire)};
        const auto rp {rPtr.load(std::memory_order_relaxed)};
        if (wp == rp) return std::nullopt;
        T res {data[mask(rp)]};
        increment(rPtr);
        return res;
    }
    ~Buffer() noexcept {
        Allocator::deallocate<T>(data, N);
    }
};

#endif //ORDERBEAMER_BUFFER_H
