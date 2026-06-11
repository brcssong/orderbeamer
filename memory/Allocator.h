//
// Created by Brian Song on 5/19/26.
//

#ifndef ORDERBEAMER_ARENA_H
#define ORDERBEAMER_ARENA_H
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

// Implements an allocator designed to be used in memory priceLevelArena formation.
namespace Allocator {
    template <typename T>
    [[nodiscard]] static T* allocate(const std::size_t amount) noexcept {
        void* allocated {mmap(nullptr, sizeof(T) * amount, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0)};
        if (allocated == MAP_FAILED) [[unlikely]] {
            const int err {errno};
            std::fprintf(stderr, "ALLOCATOR: mmap(%zu bytes) failed: %s\n", sizeof(T) * amount, std::strerror(err));
            std::abort();
        }
        return static_cast<T*>(allocated);
    }

    template <typename T>
    static void deallocate(const T* data, const std::size_t amount) noexcept {
        munmap(const_cast<T*>(data), sizeof(T) * amount);
    }
}

#endif //ORDERBEAMER_ARENA_H
