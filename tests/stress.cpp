//
// tests/stress.cpp
//
// Differential stress test for the matching core. Feeds an identical, chaotic
// op-stream (cancel-heavy + mixed quantities + wide price band) through BOTH the
// orderbeamer engine and a dead-simple, obviously-correct std::map reference, then
// asserts the two fill streams match op-for-op. Any divergence is a real bug, with
// the first one printed as a concrete repro. Touches no engine source.
//
// Build (project root):  g++ -O3 -std=c++20 -I. -o /tmp/stress tests/stress.cpp && /tmp/stress
//
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <list>
#include <map>
#include <optional>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#include "engine/statics.h"
#include "memory/Allocator.h"
#include "memory/Buffer.h"
#include "memory/OrderArena.h"
#include "memory/PriceLevelArena.h"

using PLA     = PriceLevelArena<memory_constants::PRICE_LEVEL_ARENA_SIZE>;
using OA      = OrderArena<memory_constants::OAD_ARENA_SIZE>;
using FillBuf = Buffer<FilledStub, memory_constants::LOG_BUF_SIZE>;
static constexpr std::int64_t SCALE = static_cast<std::int64_t>(engine_constants::TICK_FACTOR);
static constexpr std::size_t  N     = memory_constants::PRICE_LEVEL_ARENA_SIZE;

static inline std::uint64_t getTime() noexcept { return intrinsic_funcs::readTsc(); }

// ----- orderbeamer driver (faithful to matchingFunc) -----
struct Engine {
    Order* orderArenaPtr;
    OA orderArena; PLA buyArena; PLA sellArena;
    explicit Engine(FillBuf& fills) noexcept
        : orderArenaPtr {Allocator::allocate<Order>(memory_constants::ORDER_ARENA_SIZE)},
          orderArena {orderArenaPtr}, buyArena {orderArenaPtr, fills}, sellArena {orderArenaPtr, fills} {
        for (std::size_t i {0}; i < memory_constants::ORDER_ARENA_SIZE; ++i) {
            orderArenaPtr[i].next = (i >= memory_constants::ORDER_ARENA_SIZE - 2)
                                        ? engine_constants::MAX_32 : static_cast<std::uint32_t>(i + 1);
            orderArenaPtr[i].prev = engine_constants::MAX_32;
            orderArenaPtr[i].priceLevelArenaIdx = engine_constants::MAX_32;
        }
    }
    ~Engine() noexcept { Allocator::deallocate<Order>(orderArenaPtr, memory_constants::ORDER_ARENA_SIZE); }

    void submit(std::uint64_t userId, std::uint64_t orderId, std::int64_t price, std::uint32_t qty, bool isBuy) noexcept {
        Order order {};
        order.userId = userId; order.orderId = orderId; order.price = price; order.qty = qty; order.isBuy = isBuy;
        order.prev = order.next = order.priceLevelArenaIdx = engine_constants::MAX_32;
        if (std::uint32_t p; (p = orderArena.findHashedIndex(orderId)) != engine_constants::MAX_32) {
            const Order& original = orderArenaPtr[p];
            if (original.userId != order.userId) return;
            PLA& side = original.isBuy ? buyArena : sellArena;
            (void) side.tryCancelOrder(p, orderArena);
            return;
        }
        if (!PLA::isPriceLevelValid(order.price)) return;
        order.timestamp = getTime();
        PLA& toAdd = isBuy ? buyArena : sellArena;
        PLA& toMatch = isBuy ? sellArena : buyArena;
        toMatch.processOrder(order, isBuy, orderArena);
        if (order.qty && orderArena.checkCanAddOrder()) {
            const std::uint32_t oid {orderArena.addOrder(order)};
            toAdd.trySetPriceLevel(order.price, oid);
        }
    }
};

struct Fill { std::uint64_t maker, taker; std::int64_t price; std::uint32_t qty; };
static bool operator==(const Fill& a, const Fill& b) {
    return a.maker == b.maker && a.taker == b.taker && a.price == b.price && a.qty == b.qty;
}

// Drain this op's fills from the orderbeamer buffer (skip single-sided cancel stubs).
static void drainFills(FillBuf& fb, std::vector<Fill>& out) {
    out.clear();
    while (std::optional<FilledStub> r = fb.tryRead()) {
        const FilledStub& s = *r;
        if (s.orderId2 == engine_constants::MAX_64) continue;
        out.push_back({s.orderId1, s.orderId2, static_cast<std::int64_t>(s.filledPrice), s.filledQty});
    }
}

// ----- obviously-correct reference: std::map<priceIndex, FIFO list>, mirrors orderbeamer semantics -----
struct RefEngine {
    struct R { std::uint64_t userId, orderId; std::int64_t price; std::uint32_t qty; };
    std::map<std::size_t, std::list<R>> bids, asks;                 // priceIndex -> FIFO (time priority)
    std::unordered_map<std::uint64_t, std::pair<bool, std::size_t>> live;  // orderId -> (isBuy, priceIndex)
    std::unordered_map<std::uint64_t, std::uint64_t> owner;          // orderId -> resting order's userId

    static std::size_t idx(std::int64_t p) { return static_cast<std::uint64_t>(p) / engine_constants::TICK_FACTOR; }
    bool liveOwned(std::uint64_t orderId, std::uint64_t userId) const {
        auto it = live.find(orderId);
        return it != live.end() && owner.at(orderId) == userId;
    }
    void removeLive(std::uint64_t orderId) {
        auto it = live.find(orderId); if (it == live.end()) return;
        auto [isBuy, pidx] = it->second;
        auto& book = isBuy ? bids : asks;
        if (auto lvl = book.find(pidx); lvl != book.end()) {
            for (auto j = lvl->second.begin(); j != lvl->second.end(); ++j)
                if (j->orderId == orderId) { lvl->second.erase(j); break; }
            if (lvl->second.empty()) book.erase(lvl);
        }
        live.erase(it); owner.erase(orderId);
    }
    void submit(std::uint64_t userId, std::uint64_t orderId, std::int64_t price, std::uint32_t qty,
                bool isBuy, std::vector<Fill>& out) {
        out.clear();
        if (auto it = live.find(orderId); it != live.end()) {            // cancel-by-resend
            if (owner.at(orderId) == userId) removeLive(orderId);        // (else: ownership mismatch -> drop)
            return;
        }
        const std::size_t myIdx = idx(price);
        if (myIdx >= N) return;                                          // isPriceLevelValid
        std::uint32_t rem = qty;
        auto& opp = isBuy ? asks : bids;

        std::vector<std::size_t> keys;                                  // candidate levels in match order
        if (isBuy) { for (auto it = opp.begin(); it != opp.end() && it->first <= myIdx; ++it) keys.push_back(it->first); }
        else       { for (auto it = opp.lower_bound(myIdx); it != opp.end(); ++it) keys.push_back(it->first);
                     std::reverse(keys.begin(), keys.end()); }
        for (std::size_t k : keys) {
            if (rem == 0) break;
            auto lvl = opp.find(k); if (lvl == opp.end()) continue;
            auto& lst = lvl->second;
            for (auto j = lst.begin(); rem > 0 && j != lst.end(); ) {
                if (j->userId == userId) { ++j; continue; }             // wash: skip, keep resting
                const std::uint32_t q = std::min(j->qty, rem);
                j->qty -= q; rem -= q;
                out.push_back({j->orderId, orderId, j->price, q});
                if (j->qty == 0) { live.erase(j->orderId); owner.erase(j->orderId); j = lst.erase(j); }
                else ++j;
            }
            if (lst.empty()) opp.erase(lvl);
        }
        if (rem > 0) {                                                  // rest the remainder
            (isBuy ? bids : asks)[myIdx].push_back({userId, orderId, price, rem});
            live[orderId] = {isBuy, myIdx}; owner[orderId] = userId;
        }
    }
};

struct Op { std::uint64_t userId, orderId; std::int64_t price; std::uint32_t qty; bool isBuy; };

int main() {
#if defined(__linux__)
    { cpu_set_t s; CPU_ZERO(&s); CPU_SET(1, &s); sched_setaffinity(0, sizeof(s), &s); }
#endif
    constexpr std::size_t N_OPS = 1'000'000;
    constexpr int IDX_LO = 0, IDX_HI = 1024;        // wide price band, including the index-0 boundary level
    constexpr std::uint32_t QTY_MAX = 40;           // mixed quantities -> partial fills + sweeps
    constexpr int N_USERS = 8;                      // small pool -> ~1/8 wash collisions
    constexpr double CANCEL_FRAC = 0.45;            // cancel-heavy

    std::cout << "Generating " << N_OPS << " ops (cancel~" << int(CANCEL_FRAC*100)
              << "%, qty 1.." << QTY_MAX << ", " << (IDX_HI - IDX_LO + 1) << " price levels, "
              << N_USERS << " users)...\n";
    std::mt19937_64 rng {0xBEEF};
    std::uniform_int_distribution<int> idxDist {IDX_LO, IDX_HI};
    std::uniform_int_distribution<std::uint32_t> qtyDist {1, QTY_MAX};
    std::uniform_int_distribution<int> userDist {0, N_USERS - 1};
    std::uniform_int_distribution<int> sideDist {0, 1};
    std::uniform_real_distribution<double> coin {0.0, 1.0};

    std::vector<Op> ops; ops.reserve(N_OPS);
    std::vector<std::uint64_t> submitted;
    std::unordered_map<std::uint64_t, std::uint64_t> ownerUser;
    std::uint64_t nextId = 1;
    for (std::size_t i = 0; i < N_OPS; ++i) {
        if (!submitted.empty() && coin(rng) < CANCEL_FRAC) {            // cancel-intent: resend a recent id
            std::size_t span = std::min<std::size_t>(submitted.size(), 2000);
            std::uint64_t target = submitted[submitted.size() - 1 - std::uniform_int_distribution<std::size_t>(0, span - 1)(rng)];
            ops.push_back({ownerUser[target], target, std::int64_t(idxDist(rng)) * SCALE, qtyDist(rng), sideDist(rng) == 1});
        } else {                                                        // fresh order
            std::uint64_t id = nextId++; std::uint64_t u = static_cast<std::uint64_t>(userDist(rng));
            ops.push_back({u, id, std::int64_t(idxDist(rng)) * SCALE, qtyDist(rng), sideDist(rng) == 1});
            submitted.push_back(id); ownerUser[id] = u;
        }
    }

    std::cout << "Running differential (orderbeamer vs std::map reference)...\n";
    RefEngine ref;
    FillBuf fb; Engine ob {fb};
    std::vector<Fill> refFills, obFills;
    std::size_t totalFills = 0, realCancels = 0, divergences = 0, firstDivOp = SIZE_MAX;

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        if (ref.liveOwned(op.orderId, op.userId)) ++realCancels;
        ref.submit(op.userId, op.orderId, op.price, op.qty, op.isBuy, refFills);
        ob.submit(op.userId, op.orderId, op.price, op.qty, op.isBuy);
        drainFills(fb, obFills);
        if (refFills != obFills) {
            ++divergences;
            if (firstDivOp == SIZE_MAX) {
                firstDivOp = i;
                std::printf("  [DIVERGENCE] op #%zu: %s id=%llu user=%llu idx=%llu qty=%u\n",
                            i, op.isBuy ? "BUY" : "SELL", (unsigned long long)op.orderId,
                            (unsigned long long)op.userId, (unsigned long long)(op.price / SCALE), op.qty);
                std::printf("     reference produced %zu fill(s), orderbeamer %zu:\n", refFills.size(), obFills.size());
                std::size_t m = std::max(refFills.size(), obFills.size());
                for (std::size_t k = 0; k < m && k < 6; ++k) {
                    if (k < refFills.size()) std::printf("       ref[%zu]  maker=%llu taker=%llu px=%llu q=%u\n", k,
                        (unsigned long long)refFills[k].maker, (unsigned long long)refFills[k].taker,
                        (unsigned long long)(refFills[k].price / SCALE), refFills[k].qty);
                    if (k < obFills.size())  std::printf("       ob [%zu]  maker=%llu taker=%llu px=%llu q=%u\n", k,
                        (unsigned long long)obFills[k].maker, (unsigned long long)obFills[k].taker,
                        (unsigned long long)(obFills[k].price / SCALE), obFills[k].qty);
                }
            }
        }
        totalFills += refFills.size();
    }

    std::cout << "--------------------------------------------------\n";
    std::printf("ops: %zu   real cancels: %zu   fills: %zu   divergences: %zu\n",
                ops.size(), realCancels, totalFills, divergences);
    std::cout << (divergences == 0 ? "[PASS] orderbeamer matches the reference on every op.\n"
                                   : "[FAIL] see first divergence above.\n");
    std::cout << "--------------------------------------------------\n";

    // ----- orderbeamer-only throughput on this hard workload (times submit() only) -----
    {
        using namespace std::chrono;
        const auto c0 = intrinsic_funcs::readTsc(); const auto w0 = steady_clock::now();
        std::this_thread::sleep_for(milliseconds(50));
        const double nsPerCycle = duration<double, std::nano>(steady_clock::now() - w0).count()
                                  / static_cast<double>(intrinsic_funcs::readTsc() - c0);

        FillBuf fb2; Engine ob2 {fb2};
        std::uint64_t cyc = 0; std::vector<Fill> sink;
        for (const Op& op : ops) {
            const std::uint64_t a = intrinsic_funcs::readTsc();
            ob2.submit(op.userId, op.orderId, op.price, op.qty, op.isBuy);
            cyc += intrinsic_funcs::readTsc() - a;
            drainFills(fb2, sink);                            // untimed: keep the buffer from filling
        }
        const double secs = static_cast<double>(cyc) * nsPerCycle / 1e9;
        std::printf("orderbeamer stress throughput: %.1f M ops/sec  (%.1f ns/op, submit-only)\n",
                    static_cast<double>(N_OPS) / secs / 1e6,
                    static_cast<double>(cyc) * nsPerCycle / static_cast<double>(N_OPS));
    }

    return divergences == 0 ? 0 : 1;
}
