//
// Created by Brian Song on 5/17/26.
//

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "statics.h"
#include "gateway/gateway.h"
#include "generics/Error.h"
#include "../memory/Buffer.h"
#include "memory/Allocator.h"
#include "memory/OrderArena.h"
#include "memory/PriceLevelArena.h"

#include <pthread.h>
#include <sched.h>

void pinSelfThreadStrong(Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE>& errorBuf, const int cid) noexcept {
    cpu_set_t customCpuSet;
    CPU_ZERO(&customCpuSet);
    CPU_SET(cid, &customCpuSet);

    if (pthread_setaffinity_np(pthread_self(), sizeof(customCpuSet), &customCpuSet) != 0) {
        Error::generate(errorBuf, "PIN TO CORE: Error while pinning thread to core.", errno);
    }
}

void pinSelfThreadWeak(const int cid) noexcept {
    cpu_set_t customCpuSet;
    CPU_ZERO(&customCpuSet);
    CPU_SET(cid, &customCpuSet);

    if (pthread_setaffinity_np(pthread_self(), sizeof(customCpuSet), &customCpuSet) != 0) {
        Error::generate("PIN TO CORE: Error while pinning thread to core.", errno);
    }
}

void gatewayFunc(const int coreNo, Buffer<Order, memory_constants::SPSC_BUF_SIZE>& spsc, Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE>& errorBuf) noexcept {
    // Pin to Core 0, run on Thread 0 (network/gateway layer)
    pinSelfThreadStrong(errorBuf, coreNo);
    Gateway<Order, memory_constants::SPSC_BUF_SIZE> gateway{engine_constants::SOCKET_PATH, 8080, spsc, errorBuf};
    std::cout << "Order gateway has begun running." << "\n";
    gateway.run();
}

uint64_t getTime() noexcept {
    return intrinsic_funcs::readTsc();
}

[[noreturn]] void matchingFunc(const int coreNo, Buffer<Order, memory_constants::SPSC_BUF_SIZE>& spsc, Buffer<FilledStub, memory_constants::LOG_BUF_SIZE>& filledBuf, Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE>& errorBuf) noexcept {
    // Pin to Core 1, run on Thread 1 (matching layer)
    pinSelfThreadStrong(errorBuf, coreNo);
    Order* orderArenaPtr {Allocator::allocate<Order>(memory_constants::ORDER_ARENA_SIZE)};

    for (std::size_t i {0}; i < memory_constants::ORDER_ARENA_SIZE; i++) {
        orderArenaPtr[i].next = (i == memory_constants::ORDER_ARENA_SIZE - 1 || i == memory_constants::ORDER_ARENA_SIZE - 2) ? engine_constants::MAX_32 : i + 1;
        orderArenaPtr[i].prev = engine_constants::MAX_32;
        orderArenaPtr[i].priceLevelArenaIdx = engine_constants::MAX_32;
    }

    OrderArena<memory_constants::OAD_ARENA_SIZE> orderArena{orderArenaPtr};
    PriceLevelArena<memory_constants::PRICE_LEVEL_ARENA_SIZE> buyArena {orderArenaPtr, filledBuf};
    PriceLevelArena<memory_constants::PRICE_LEVEL_ARENA_SIZE> sellArena {orderArenaPtr, filledBuf};
    std::cout << "Matching engine has begun running." << "\n";
    while (true) {
        if (std::optional<Order> res = spsc.tryRead(); res.has_value()) {
            Order order {res.value()};
            if (std::uint32_t orderPtr; (orderPtr = orderArena.findHashedIndex(order.orderId)) != engine_constants::MAX_32) {
                const Order& originalOrder = orderArenaPtr[orderPtr];
                if (originalOrder.userId != order.userId) [[unlikely]] {
                    Error::generate(errorBuf, "ORDER CANCEL: Cancel order request could not be verified. Check user IDs.", 0);
                    continue;
                }
                // Begin cancel flow, userId and orderId are the same
                if (auto& arena {originalOrder.isBuy ? buyArena : sellArena}; !arena.tryCancelOrder(orderPtr, orderArena)) {
                    Error::generate(errorBuf, "ORDER CANCEL: Cancel order request was not successfully processed.", 0);
                }

                continue;
            }
            if (!PriceLevelArena<memory_constants::PRICE_LEVEL_ARENA_SIZE>::isPriceLevelValid(order.price)) [[unlikely]] {
                Error::generate(errorBuf, "ORDER MATCH: Failed to add price level correctly.", 0);
                continue;
            }
            order.timestamp = getTime();
            auto& priceLevelsArenaToAdd {order.isBuy ? buyArena : sellArena};
            auto& priceLevelsArenaToMatchAgainst {order.isBuy ? sellArena : buyArena};
            // Process/fill orders via pre-defined rules
            priceLevelsArenaToMatchAgainst.processOrder(order, order.isBuy, orderArena);
            if (order.qty) {
                if (orderArena.checkCanAddOrder()) {
                    const uint32_t oid {orderArena.addOrder(order)};
                    priceLevelsArenaToAdd.trySetPriceLevel(order.price, oid);
                } else {
                    Error::generate(errorBuf, "Failed to add order due to fully filled database.", 0);
                }
            }
        } else {
            intrinsic_funcs::efficientPause();
        }
    }
    // Allocator::deallocate<Order>(order_arena, memory_constants::ORDER_ARENA_SIZE);
    // ^reclaimed by the operating system (deliberately to simplify the execution flow)
}

void loggingHelper(std::ofstream& lout, const std::uint64_t orderId, const double freq, const std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>& data) noexcept {
    lout << std::left << std::fixed << std::setprecision(2) << "ORDER FILLED - TIME " << std::setw(20) << static_cast<long double>(std::get<2>(data) - std::get<1>(data)) / freq << " - USER " << std::setw(20) << std::get<0>(data) << " - ORDER " << std::setw(20) << orderId << " - FILLED A TOTAL OF " << std::setw(20) << std::get<4>(data) << " SHARES AT AN AVERAGE OF " << std::setw(20) << static_cast<long double>(std::get<5>(data)) / std::get<4>(data) << " PER SHARE\n";
}

[[noreturn]] void loggingFunc(int coreNo, Buffer<FilledStub, memory_constants::LOG_BUF_SIZE>& filledBuf, double freq) noexcept {
    // {User_Id, Original_timestamp, Last updated timestamp, Remaining quantity, Filled quantity, Total fill price}
    pinSelfThreadWeak(coreNo);
    std::unordered_map<std::uint64_t, std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>> oidToTimingResults;
    std::ofstream logFile{"completed_orders.txt"};
    while (true) {
        if (std::optional<FilledStub> res = filledBuf.tryRead(); res.has_value()) {
            FilledStub filledStub {res.value()};
            std::get<0>(oidToTimingResults[filledStub.orderId1]) = filledStub.userId1;
            std::get<1>(oidToTimingResults[filledStub.orderId1]) = filledStub.order1Timestamp;
            std::get<2>(oidToTimingResults[filledStub.orderId1]) = filledStub.filledTimestamp;
            std::get<3>(oidToTimingResults[filledStub.orderId1]) = filledStub.user1QtyRemain;
            std::get<4>(oidToTimingResults[filledStub.orderId1]) += filledStub.filledQty;
            std::get<5>(oidToTimingResults[filledStub.orderId1]) += filledStub.filledPrice * filledStub.filledQty;
            if (!std::get<3>(oidToTimingResults[filledStub.orderId1])) {
                loggingHelper(logFile, filledStub.orderId1, freq, oidToTimingResults[filledStub.orderId1]);
                oidToTimingResults.erase(filledStub.orderId1);
            }

            if (filledStub.orderId2 != engine_constants::MAX_64) {
                std::get<0>(oidToTimingResults[filledStub.orderId2]) = filledStub.userId2;
                std::get<1>(oidToTimingResults[filledStub.orderId2]) = filledStub.order2Timestamp;
                std::get<2>(oidToTimingResults[filledStub.orderId2]) = filledStub.filledTimestamp;
                std::get<3>(oidToTimingResults[filledStub.orderId2]) = filledStub.user2QtyRemain;
                std::get<4>(oidToTimingResults[filledStub.orderId2]) += filledStub.filledQty;
                std::get<5>(oidToTimingResults[filledStub.orderId2]) += filledStub.filledPrice * filledStub.filledQty;
                if (!std::get<3>(oidToTimingResults[filledStub.orderId2])) {
                    loggingHelper(logFile, filledStub.orderId2, freq, oidToTimingResults[filledStub.orderId2]);
                    oidToTimingResults.erase(filledStub.orderId2);
                }
            }
        } else {
            intrinsic_funcs::efficientPause();
        }
    }
}

[[noreturn]] void errorFunc(const int coreNo, Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE>& errorBuf) noexcept {
    pinSelfThreadWeak(coreNo);
    while (true) {
        if (std::optional<ErrorRecord> res = errorBuf.tryRead(); res.has_value()) {
            Error::output(res.value().errorString, res.value().errorNumber);
        } else {
            intrinsic_funcs::efficientPause();
        }
    }
}

// Calibrate for efficient clock reads
long double calibrateTSC() {
    using namespace std::chrono;

    const auto startTsc = intrinsic_funcs::readTsc();
    const auto startTime = steady_clock::now();

    std::this_thread::sleep_for(milliseconds(100));

    const auto endTsc = intrinsic_funcs::readTsc();
    const auto endTime = steady_clock::now();

    const auto elapsedNs = duration_cast<nanoseconds>(endTime - startTime).count();
    const auto elapsedCycles = endTsc - startTsc;

    return static_cast<long double>(elapsedCycles) / elapsedNs;
}

int main() {
    pinSelfThreadWeak(0);

    Buffer<Order, memory_constants::SPSC_BUF_SIZE> queueMessageBuf{};
    Buffer<FilledStub, memory_constants::LOG_BUF_SIZE> filledBuf{};
    Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE> gatewayErrorBuf{};
    Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE> matchingErrorBuf{};

    long double tscMult {calibrateTSC()};

    std::thread gatewayThread(&gatewayFunc, 1, std::ref(queueMessageBuf), std::ref(gatewayErrorBuf));
    std::thread matchingThread(&matchingFunc, 2, std::ref(queueMessageBuf), std::ref(filledBuf), std::ref(matchingErrorBuf));
    std::thread loggingThread(&loggingFunc, 3, std::ref(filledBuf), tscMult);
    std::thread gatewayErrorThread(&errorFunc, 4, std::ref(gatewayErrorBuf));
    std::thread matchingErrorThread(&errorFunc, 5, std::ref(matchingErrorBuf));

    gatewayThread.join();
    matchingThread.join();
    loggingThread.join();
    gatewayErrorThread.join();
    matchingErrorThread.join();
}
