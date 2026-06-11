//
// Created by Brian Song on 5/20/26.
//

#ifndef ORDERBEAMER_ARCHBITSET_H
#define ORDERBEAMER_ARCHBITSET_H
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "Allocator.h"
#include "Buffer.h"
#include "OrderArena.h"
#include "engine/statics.h"

[[nodiscard]] consteval static std::size_t calculateNumWords(std::size_t N) noexcept {
    std::size_t curr {0};
    while (N > 1) {
        curr += N;
        N = (N + 63) >> 6;
    }
    // +1 reserves the single-word root level
    return curr + 1;
}

[[nodiscard]] consteval static std::array<std::size_t, engine_constants::MAX_HIERARCHICAL_LEVELS> getLevelIndex(std::size_t N) noexcept {
    // level 0: index 0
    // level 1: index N
    // level 2: index N + (N + 63) >> 6, and so on
    std::size_t curr {0};
    std::size_t level {0};
    std::array<std::size_t, engine_constants::MAX_HIERARCHICAL_LEVELS> arr{};
    arr.fill(static_cast<std::size_t>(-1));
    while (N > 1) {
        arr[level] = curr;
        curr += N;
        level++;
        N = (N + 63) >> 6;
    }
    arr[level] = curr;
    return arr;
}

[[nodiscard]] constexpr static std::uint32_t getHeadOfDLL(const std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value >> 32);
}

[[nodiscard]] constexpr static std::uint32_t getTailOfDLL(const std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value & engine_constants::BOT_32BMASK);
}

[[nodiscard]] consteval static int calcNumLevels(std::size_t N) noexcept {
    int levels {0};
    while (N > 1) {
        levels++;
        N = (N + 63) >> 6;
    }
    return levels + 1;
}

// Implements a hierarchical bitset on uint64_t words (8 bytes = 64b).
// Handles underlying type `struct Order.`
// N is the # of distinct price levels to support
// At each price level, a 64-bit word stores (uint32_t head_idx << 4) | (uint32_t tail_idx << 4) to DLL
// Also, a separate bitset is made for each side (BUY and SELL).
template <std::size_t N>
class PriceLevelArena {
    using u64 = std::uint64_t;
    using u32 = std::uint32_t;

    u64* priceLevelArena{Allocator::allocate<u64>(calculateNumWords(N))};
    Order* orderArena;
    std::array<std::size_t, engine_constants::MAX_HIERARCHICAL_LEVELS> levelIndices {getLevelIndex(N)};
    int numLevels {calcNumLevels(N)};
    Buffer<FilledStub, memory_constants::LOG_BUF_SIZE>& filledBuf;

    void removeOrderIndexFromPriceIndex(const std::size_t priceIndex, Order& customOrder, bool isHead) noexcept {
        // Assume that there exist orders at the desired price index
        u64& priceLevelNode {priceLevelArena[priceIndex]};
        Order& toBeRemovedOrder {customOrder};
        if (toBeRemovedOrder.next == engine_constants::MAX_32) {
            if (isHead) {
                priceLevelNode = engine_constants::MAX_64;
                unsetBitmap(priceIndex);
            } else {
                priceLevelNode &= engine_constants::TOP_32BMASK;
                priceLevelNode |= toBeRemovedOrder.prev;
            }
        } else {
            if (isHead) {
                priceLevelNode &= engine_constants::BOT_32BMASK;
                priceLevelNode |= static_cast<u64>(toBeRemovedOrder.next) << 32;
                Order& nextOrderInLine {orderArena[toBeRemovedOrder.next]};
                nextOrderInLine.prev = engine_constants::MAX_32;
            }
        }
        if (toBeRemovedOrder.prev != engine_constants::MAX_32) {
            orderArena[toBeRemovedOrder.prev].next = toBeRemovedOrder.next;
        }
        if (toBeRemovedOrder.next != engine_constants::MAX_32) {
            orderArena[toBeRemovedOrder.next].prev = toBeRemovedOrder.prev;
        }
        toBeRemovedOrder.prev = engine_constants::MAX_32;
        toBeRemovedOrder.next = engine_constants::MAX_32;
        toBeRemovedOrder.priceLevelArenaIdx = engine_constants::MAX_32;
    }

    void setBitmap(const std::size_t priceIndex) noexcept {
        std::size_t level {1};
        std::size_t offset {priceIndex};
        while (levelIndices[level] != static_cast<std::size_t>(-1)) {
            const std::size_t nextLevelIndex {(offset >> 6) + levelIndices[level]};
            if (priceLevelArena[nextLevelIndex] & (1ULL << (offset & 63))) {
                return;
            }
            priceLevelArena[nextLevelIndex] |= 1ULL << (offset & 63);
            level++;
            offset >>= 6;
        }
    }

    void unsetBitmap(const std::size_t priceIndex) noexcept {
        std::size_t level {1};
        std::size_t offset {priceIndex};
        // The caller has already emptied this leaf; each higher level then clears only once its child word becomes all-zero.
        bool nextOneCan {true};
        while (levelIndices[level] != static_cast<std::size_t>(-1)) {
            const std::size_t nextLevelIndex {(offset >> 6) + levelIndices[level]};
            if (!(priceLevelArena[nextLevelIndex] & (1ULL << (offset & 63)))) {
                return;
            }
            if (nextOneCan) {
                priceLevelArena[nextLevelIndex] ^= 1ULL << (offset & 63);
            }
            nextOneCan = !priceLevelArena[nextLevelIndex];
            level++;
            offset >>= 6;
        }
    }

    [[nodiscard]] std::size_t findNextIndex(bool greater, const std::size_t priceIndex) const noexcept {
        // Phase 0: Keep scanning if already exists at this index (wash sale)
        // Phase 1: Scan up
        int currentLevel {1};
        std::size_t currentPriceIndexStart {levelIndices[1]};
        std::size_t currentWordIdx {priceIndex >> 6};
        std::size_t currentOffset {priceIndex & 63};
        u64 currentWord {engine_constants::MAX_64};
        // Start from leaf, count trailing zeroes, and every time the word is 0, jump up 1 layer. When found, scan back down.
        while (currentPriceIndexStart != -1) {
            // Create bitmask
            const u64 bitmask {greater ? (currentOffset == 63 ? 0ULL : ~((1ULL << (currentOffset + 1)) - 1ULL)) : (1ULL << currentOffset) - 1ULL};
            if ((currentWord = {priceLevelArena[currentPriceIndexStart + currentWordIdx] & bitmask}); currentWord) {
                break;
            }
            currentOffset = currentWordIdx & 63;
            currentPriceIndexStart = levelIndices[++currentLevel];
            currentWordIdx >>= 6;
        }

        if (!currentWord) {
            // No match found at all in the bitset. Return early.
            return -1;
        }

        // Phase 2: Scan down
        while (true) {
            const std::size_t bit = greater ? std::countr_zero(currentWord) : 63 - std::countl_zero(currentWord);
            const std::size_t logicalBit = (currentWordIdx << 6) | bit;
            if (currentLevel == 1) return logicalBit;
            currentWordIdx = logicalBit;
            currentWord = priceLevelArena[levelIndices[--currentLevel] + currentWordIdx];
        }

        return -1;
    }

    static std::size_t getPriceIndexFromPriceLevel(const u64 priceLevel) noexcept {
        return priceLevel / engine_constants::TICK_FACTOR;
    }
public:
    PriceLevelArena(Order* orderArenaPtr, Buffer<FilledStub, memory_constants::LOG_BUF_SIZE>& filledBuf) noexcept : orderArena{orderArenaPtr}, filledBuf{filledBuf} {
        // Set first N
        for (std::size_t i {0}; i < N; i++) {
            priceLevelArena[i] = engine_constants::MAX_64;
        }
    }

    // delete default copy ctors
    PriceLevelArena (const PriceLevelArena& priceArena) = delete;
    PriceLevelArena (PriceLevelArena& priceArena) = delete;

    [[nodiscard]] static bool isPriceLevelValid(const u64 priceLevel) noexcept {
        return getPriceIndexFromPriceLevel(priceLevel) < N && !(priceLevel % engine_constants::TICK_FACTOR);
    }

    // Try to add a price to the desired price level.
    void trySetPriceLevel(const u64 priceLevel_, const u32 orderArenaIdx_) noexcept {
        const std::size_t priceIndex {getPriceIndexFromPriceLevel(priceLevel_)};
        const u64 orderArenaIdx {orderArenaIdx_};
        Order& toAddOrder {orderArena[orderArenaIdx]};
        u64& priceLevelNode {priceLevelArena[priceIndex]};
        if (priceLevelNode == engine_constants::MAX_64) {
            priceLevelNode = (orderArenaIdx << 32) | (orderArenaIdx);
            toAddOrder.prev = engine_constants::MAX_32;
            toAddOrder.next = engine_constants::MAX_32;
            // Update the rest of the bitmap with enough values
            setBitmap(priceIndex);
        } else {
            // Update original tail's nextPtr
            const u64 tailPtr {priceLevelNode & ((1ULL << 32) - 1)};
            priceLevelNode &= engine_constants::TOP_32BMASK;
            priceLevelNode |= orderArenaIdx;
            Order& oldTail {orderArena[tailPtr]};
            oldTail.next = orderArenaIdx_;
            toAddOrder.prev = tailPtr;
            toAddOrder.next = engine_constants::MAX_32;
        }
        toAddOrder.priceLevelArenaIdx = static_cast<u32>(priceIndex);

        // Corresponding priceLevel element is already updated via reference.
    }

    // Matches price level and tries to clear as many orders as possible.
    // Caller should specify:
    // - Sell instance on a Buy-side PriceLevelArena (greater = False) start from top and sweep down
    // - Buy instance on a Sell-side PriceLevelArena (greater = True) start from bottom and sweep up
   void processOrder(Order& matchingOrder, const bool greater, OrderArena<memory_constants::OAD_ARENA_SIZE>& oadRef) noexcept {
        // We already assume that the side taken is opposing (e.g. BUY vs SELL)
        // If there is still remaining quantity, updates the Order struct itself.
        // Caller is expected to place this into the other side's bitset.
        const std::size_t matchingPriceIndex {getPriceIndexFromPriceLevel(matchingOrder.price)};
        // findNextIndex is exclusive on the provided level, so boundary level inclusively so that
        // an order at index 0 (buy) / N-1 (sell) is not skipped.
        std::size_t index {
            greater ? (priceLevelArena[0] != engine_constants::MAX_64 ? std::size_t{0} : findNextIndex(true, 0)) :
            (priceLevelArena[N - 1] != engine_constants::MAX_64 ? N - 1 : findNextIndex(false, N - 1))
        };
        while (matchingOrder.qty) {
            if (index == -1) [[unlikely]] {
                // Finished (entire order book has been cleared).
                break;
            }
            if ((greater && index > matchingPriceIndex) || (!greater && index < matchingPriceIndex)) {
                // Finished (exceeded the matching mid-level priceIndex).
                break;
            }

            u32 orderIdx {getHeadOfDLL(priceLevelArena[index])};
            while (matchingOrder.qty && orderIdx != engine_constants::MAX_32) {
                Order& user1Order {orderArena[orderIdx]};
                const u32 nextOrderIdx {user1Order.next};
                // Prevent wash sales by checking user IDs
                if (user1Order.userId != matchingOrder.userId) {
                    const std::uint32_t quantityToProcess {std::min(user1Order.qty, matchingOrder.qty)};
                    user1Order.qty -= quantityToProcess;
                    matchingOrder.qty -= quantityToProcess;

                    // Create filled stub
                    FilledStub fsStub {};
                    fsStub.filledTimestamp = intrinsic_funcs::readTsc();
                    fsStub.userId1 = user1Order.userId;
                    fsStub.orderId1 = user1Order.orderId;
                    fsStub.userId2 = matchingOrder.userId;
                    fsStub.orderId2 = matchingOrder.orderId;
                    fsStub.filledQty = quantityToProcess;
                    fsStub.user1QtyRemain = user1Order.qty;
                    fsStub.filledPrice = user1Order.price;
                    fsStub.user2QtyRemain = matchingOrder.qty;
                    fsStub.order1Timestamp = user1Order.timestamp;
                    fsStub.order2Timestamp = matchingOrder.timestamp;
                    while (!filledBuf.tryWrite(fsStub)) {
                        intrinsic_funcs::efficientPause();
                    }

                    if (!user1Order.qty) {
                        removeOrderIndexFromPriceIndex(index, user1Order, orderIdx == getHeadOfDLL(priceLevelArena[index]));
                        // Remove this order from the book
                        oadRef.removeOrderFromOrderArena(orderIdx);
                    }
                }
                orderIdx = nextOrderIdx;
            }

            index = findNextIndex(greater, index);
        }
    }

    [[nodiscard]] bool tryCancelOrder(const u32 orderArenaIdx, OrderArena<memory_constants::OAD_ARENA_SIZE>& oadRef) noexcept {
        Order& target {orderArena[orderArenaIdx]};
        const std::size_t index {target.priceLevelArenaIdx};
        const bool isHead {target.prev == engine_constants::MAX_32};
        FilledStub fsStub {};
        fsStub.filledTimestamp = intrinsic_funcs::readTsc();
        fsStub.userId1 = target.userId;
        fsStub.orderId1 = target.orderId;
        fsStub.userId2 = 0;
        fsStub.orderId2 = engine_constants::MAX_64;
        fsStub.filledQty = 0;
        fsStub.user1QtyRemain = 0;
        fsStub.filledPrice = 0;
        fsStub.user2QtyRemain = 0;
        fsStub.order1Timestamp = target.timestamp;
        fsStub.order2Timestamp = 0;
        removeOrderIndexFromPriceIndex(index, target, isHead);
        oadRef.removeOrderFromOrderArena(orderArenaIdx);
        while (!filledBuf.tryWrite(fsStub)) {
            intrinsic_funcs::efficientPause();
        }
        return true;
    }

    ~PriceLevelArena() {
        Allocator::deallocate<u64>(priceLevelArena, calculateNumWords(N));
    }

};

#endif //ORDERBEAMER_ARCHBITSET_H
