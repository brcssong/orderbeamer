//
// Created by Brian Song on 5/21/26.
//

#ifndef ORDERBEAMER_ORDERARENA_H
#define ORDERBEAMER_ORDERARENA_H
#include <cstddef>
#include <cstdint>
#include "Allocator.h"
#include "engine/statics.h"

template <std::size_t OADT> requires ((OADT & (OADT - 1)) == 0)
class OrderArena {
    using u64 = std::uint64_t;
    using u32 = std::uint32_t;
    Order* orderArena;
    OADRecord* oadArena {Allocator::allocate<OADRecord>(OADT)};
    u32 nextFreeIdx {0};
    int currentOADArenaSize {0};
public:
    explicit OrderArena(Order* orderArenaPtr) noexcept : orderArena{orderArenaPtr} {
        for (std::size_t i {0}; i < OADT; i++) {
            oadArena[i] = OADRecord {0,  engine_constants::MAX_32};
        }
    }

    [[nodiscard]] bool checkCanAddOrder() const noexcept {
        return nextFreeIdx != engine_constants::MAX_32 && !orderArena[nextFreeIdx].qty && currentOADArenaSize < OADT;
    }

    [[nodiscard]] u32 findHashedIndex(const u64 orderId) const noexcept {
        u32 hashedIndex {static_cast<u32>(orderId & (OADT - 1))};
        for (int i {0}; i < OADT; i++) {
            if (oadArena[hashedIndex].orderArenaId == engine_constants::MAX_32) {
                break;
            }
            if (oadArena[hashedIndex].orderId == orderId && oadArena[hashedIndex].orderArenaId != engine_constants::MAX_32 - 1) {
                return oadArena[hashedIndex].orderArenaId;
            }
            ++hashedIndex;
            hashedIndex &= (OADT - 1);
        }
        return engine_constants::MAX_32;
    }

    u32 addOrder(const Order& order) noexcept {
        const u32 idx {nextFreeIdx};
        // Deallocated orders used prev/next to point to free memory locations
        nextFreeIdx = orderArena[idx].next;

        orderArena[idx] = order;
        // Allocate this order ID into the OAD arena
        // Assumes that OADT given is a Power of 2
        u32 hashedIndex {static_cast<u32>(order.orderId & (OADT - 1))};
        for (int i {0}; i < OADT; i++) {
            if (oadArena[hashedIndex].orderArenaId == engine_constants::MAX_32 || oadArena[hashedIndex].orderArenaId == engine_constants::MAX_32 - 1) {
                // Found a valid insertion location, then place the order reference here
                oadArena[hashedIndex].orderId = order.orderId;
                oadArena[hashedIndex].orderArenaId = idx;
                break;
            }
            ++hashedIndex;
            hashedIndex &= (OADT - 1);
        }
        ++currentOADArenaSize;
        return idx;
    }

    void removeOrderFromOADTArena(const u64 orderId) noexcept {
        u32 hashedIndex {static_cast<u32>(orderId & (OADT - 1))};
        for (int i {0}; i < OADT; i++) {
            if (oadArena[hashedIndex].orderArenaId == engine_constants::MAX_32) {
                return;
            }
            if (oadArena[hashedIndex].orderId == orderId && oadArena[hashedIndex].orderArenaId != engine_constants::MAX_32 - 1) {
                // A valid order was found. Remove it.
                oadArena[hashedIndex].orderArenaId = engine_constants::MAX_32 - 1;
                return;
            }
            ++hashedIndex;
            hashedIndex &= (OADT - 1);
        }
    }

    void removeOrderFromOrderArena(const u32 orderArenaIdx) noexcept {
        // Price-level relevant pointers and members have already been updated
        Order& toBeRemoved {orderArena[orderArenaIdx]};
        toBeRemoved.qty = 0;
        toBeRemoved.prev = engine_constants::MAX_32;
        toBeRemoved.next = nextFreeIdx;
        nextFreeIdx = orderArenaIdx;
        --currentOADArenaSize;
        removeOrderFromOADTArena(toBeRemoved.orderId);
    }
};

#endif //ORDERBEAMER_ORDERARENA_H
