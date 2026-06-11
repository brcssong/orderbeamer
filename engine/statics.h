//
// Created by Brian Song on 5/17/26.
//

#ifndef ORDERBEAMER_STATICS_H
#define ORDERBEAMER_STATICS_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace engine_constants {
    inline constexpr auto SOCKET_PATH = "/tmp/orderbeamer.sock";
    inline constexpr auto LOG_FILENAME = "completed_orders.txt";
    inline constexpr int PORT = 8080;
    // Assume we are dealing with two digits of precision after the decimal point only.
    inline constexpr std::uint64_t TICK_FACTOR = 1e7; // For price -> price_level index conversion
    inline constexpr std::uint64_t MAX_64 = 0xFFFFFFFFFFFFFFFFULL;
    inline constexpr std::uint32_t MAX_32 = 0xFFFFFFFF;
    inline constexpr std::uint64_t TOP_32BMASK = 0xFFFFFFFF00000000ULL;
    inline constexpr std::uint64_t BOT_32BMASK = 0x00000000FFFFFFFFULL;
    inline constexpr std::size_t MAX_HIERARCHICAL_LEVELS = 16;
}


#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace intrinsic_funcs {
    inline void efficientPause() noexcept {
        #if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
        #elif defined(__aarch64__) || defined(_M_ARM64)
            __asm__ __volatile__ ("yield" ::: "memory"); // ARM64 equivalent
        #elif defined(__i386__)
            __asm__ __volatile__ ("pause" ::: "memory");
        #endif
    }

    [[nodiscard]] inline std::uint64_t readTsc() noexcept {
        #if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
            return __rdtsc();                                   // CPU cycles
        #elif defined(__aarch64__)
            std::uint64_t v;
            asm volatile("mrs %0, cntvct_el0" : "=r"(v));       // ARM virtual counter
            return v;
        #endif
    }
}

namespace memory_constants {
    inline constexpr std::size_t SPSC_BUF_SIZE = 65'536;
    inline constexpr std::size_t ORDER_ARENA_SIZE = 1'048'576;
    inline constexpr std::size_t PRICE_LEVEL_ARENA_SIZE = 1'048'576;
    inline constexpr std::size_t OAD_ARENA_SIZE = 2'097'152;
    inline constexpr std::size_t LOG_BUF_SIZE = 2'097'152;
    inline constexpr std::size_t ERROR_BUF_SIZE = 512;
}

namespace gateway_constants {
    inline constexpr std::size_t MAX_EPOLL_EVENTS = 128;
    inline constexpr std::size_t MAX_CONNECTIONS = 1'000;
}

struct alignas(64) Order {
    std::uint64_t timestamp;
    std::uint64_t trueTimestamp;
    std::uint64_t userId;
    std::uint64_t orderId;
    std::int64_t price;
    std::uint32_t qty;
    std::uint32_t prev;
    std::uint32_t next;
    std::uint32_t priceLevelArenaIdx;
    bool isBuy;
};

struct alignas(128) FilledStub {
    std::uint64_t filledTimestamp;
    std::uint64_t userId1;
    std::uint64_t orderId1;
    std::uint64_t order1Timestamp;
    std::uint64_t userId2;
    std::uint64_t orderId2;
    std::uint64_t order2Timestamp;
    std::uint64_t filledPrice;
    std::uint32_t filledQty;
    std::uint32_t user1QtyRemain;
    std::uint32_t user2QtyRemain;
};

struct alignas(16) OADRecord {
    // Open Addressing Record
    std::uint64_t orderId;
    std::uint32_t orderArenaId;
};

struct alignas(8) ErrorRecord {
    // Error Logging Record
    std::string_view errorString;
    int errorNumber;
};

#endif //ORDERBEAMER_STATICS_H
