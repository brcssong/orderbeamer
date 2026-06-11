//
// tests/bench.cpp
//
// Correctness + throughput + latency-distribution harness for the orderbeamer
// matching core (runCorrectnessTest / runBenchmark / runStressTest), driving the
// SAME per-order path as engine.cpp's matchingFunc() via a thin `Engine` wrapper —
// WITHOUT modifying any engine source.
//
// Build (from project root):
//   clang++ -O3 -std=c++20 -I. -o /tmp/ob_bench tests/bench.cpp && /tmp/ob_bench
// or build the `bench` target via CMake (Docker/Linux toolchain for pinned numbers).
//
#ifndef _GNU_SOURCE
#define _GNU_SOURCE     // sched_setaffinity / cpu_set_t on Linux
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <random>
#include <thread>
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

// The engine buckets price by TICK_FACTOR, so to give every integer price its own
// level, scale incoming prices by TICK_FACTOR.
static constexpr std::int64_t SCALE = static_cast<std::int64_t>(engine_constants::TICK_FACTOR);

static inline std::uint64_t getTime() noexcept { return intrinsic_funcs::readTsc(); }

// Owns the arenas and replicates matchingFunc()'s per-order path exactly.
struct Engine {
    Order* orderArenaPtr;
    OA     orderArena;
    PLA    buyArena;
    PLA    sellArena;

    explicit Engine(FillBuf& fills) noexcept
        : orderArenaPtr {Allocator::allocate<Order>(memory_constants::ORDER_ARENA_SIZE)},
          orderArena    {orderArenaPtr},
          buyArena      {orderArenaPtr, fills},
          sellArena     {orderArenaPtr, fills} {
        for (std::size_t i {0}; i < memory_constants::ORDER_ARENA_SIZE; ++i) {
            orderArenaPtr[i].next = (i == memory_constants::ORDER_ARENA_SIZE - 1 ||
                                     i == memory_constants::ORDER_ARENA_SIZE - 2)
                                        ? engine_constants::MAX_32
                                        : static_cast<std::uint32_t>(i + 1);
            orderArenaPtr[i].prev = engine_constants::MAX_32;
            orderArenaPtr[i].priceLevelArenaIdx = engine_constants::MAX_32;
        }
    }
    ~Engine() noexcept { Allocator::deallocate<Order>(orderArenaPtr, memory_constants::ORDER_ARENA_SIZE); }

    // Faithful copy of matchingFunc()'s body for one inbound order.
    void submit(std::uint64_t userId, std::uint64_t orderId, std::int64_t price,
                std::uint32_t qty, bool isBuy) noexcept {
        Order order {};
        order.userId  = userId;
        order.orderId = orderId;
        order.price   = price;
        order.qty     = qty;
        order.isBuy   = isBuy;
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
        PLA& toAdd   = isBuy ? buyArena  : sellArena;
        PLA& toMatch = isBuy ? sellArena : buyArena;
        toMatch.processOrder(order, isBuy, orderArena);
        if (order.qty && orderArena.checkCanAddOrder()) {
            const std::uint32_t oid {orderArena.addOrder(order)};
            toAdd.trySetPriceLevel(order.price, oid);
        }
    }
};

// ---- tiny test harness (fires under -O3, unlike assert) ----
static int g_failures {0};
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "  [FAIL] " << (msg) << "\n"; ++g_failures; } } while (0)

static std::vector<FilledStub> drain(FillBuf& buf) {
    std::vector<FilledStub> out;
    while (std::optional<FilledStub> r = buf.tryRead()) out.push_back(*r);
    return out;
}

// Pin the calling thread to a core (Linux only; no-op elsewhere, as on macOS).
static bool pinToCore(int core) {
#if defined(__linux__)
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(core, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void) core; return false;
#endif
}

// Measure ns-per-tick of readTsc() against a steady wall clock.
static double calibrateNsPerCycle() {
    using namespace std::chrono;
    const auto c0 = intrinsic_funcs::readTsc();
    const auto t0 = steady_clock::now();
    std::this_thread::sleep_for(milliseconds(100));
    const auto c1 = intrinsic_funcs::readTsc();
    const auto t1 = steady_clock::now();
    const double ns     = duration<double, std::nano>(t1 - t0).count();
    const double cycles = static_cast<double>(c1 - c0);
    return cycles > 0.0 ? ns / cycles : 1.0;
}

struct LatStats { double p50, p90, p99, p999, maxNs; };
static LatStats percentiles(std::vector<std::uint64_t>& cyc, double nsPerCycle) {
    std::sort(cyc.begin(), cyc.end());
    const std::size_t n = cyc.size();
    auto at = [&](double p) -> double {
        std::size_t i = static_cast<std::size_t>(p * static_cast<double>(n));
        if (i >= n) i = n - 1;
        return static_cast<double>(cyc[i]) * nsPerCycle;
    };
    return { at(0.50), at(0.90), at(0.99), at(0.999), static_cast<double>(cyc.back()) * nsPerCycle };
}

void runCorrectnessTest() {
    std::cout << "Running Logic/Correctness Verification...\n";

    { // SCENARIO 1: Simple Fill
        FillBuf fb; Engine e {fb};
        e.submit(1, 101, 100 * SCALE, 10, false);
        CHECK(drain(fb).empty(), "Passive Sell should not match yet");
        e.submit(2, 201, 100 * SCALE, 5, true);
        auto f1 = drain(fb);
        CHECK(f1.size() == 1, "Buy should trigger 1 trade event");
        CHECK(!f1.empty() && f1[0].filledQty == 5, "Trade qty should be 5");
        CHECK(!f1.empty() && f1[0].user1QtyRemain == 5, "Sell should have 5 remaining");
        CHECK(!f1.empty() && f1[0].order1Timestamp != 0 && f1[0].order2Timestamp != 0 &&
              f1[0].filledTimestamp >= f1[0].order1Timestamp &&
              f1[0].filledTimestamp >= f1[0].order2Timestamp,
              "Logged timestamps must be set and ordered (entry <= fill)");
        e.submit(3, 202, 100 * SCALE, 5, true);
        auto f2 = drain(fb);
        CHECK(!f2.empty() && f2[0].user1QtyRemain == 0, "Sell should be fully filled");
        std::cout << "  [PASS] Scenario 1: Basic Fill\n";
    }
    { // SCENARIO 2: Price Priority
        FillBuf fb; Engine e {fb};
        e.submit(1, 101, 100 * SCALE, 10, false);
        e.submit(1, 102, 101 * SCALE, 10, false);
        drain(fb);
        e.submit(2, 201, 102 * SCALE, 5, true);
        auto f = drain(fb);
        CHECK(!f.empty() && f[0].filledPrice == static_cast<std::uint64_t>(100 * SCALE),
              "Buyer should match best price (100), not 101");
        std::cout << "  [PASS] Scenario 2: Price Priority\n";
    }
    { // SCENARIO 3: Time Priority (FIFO)
        FillBuf fb; Engine e {fb};
        e.submit(1, 101, 100 * SCALE, 10, false);
        e.submit(1, 102, 100 * SCALE, 10, false);
        drain(fb);
        e.submit(2, 201, 100 * SCALE, 10, true);
        auto f = drain(fb);
        CHECK(!f.empty() && f[0].orderId1 == 101, "FIFO: oldest (101) should match first");
        std::cout << "  [PASS] Scenario 3: Time Priority\n";
    }
    { // SCENARIO 4 (engine extension): Cancel
        FillBuf fb; Engine e {fb};
        e.submit(1, 101, 100 * SCALE, 10, false);
        drain(fb);
        e.submit(1, 101, 0, 0, false);                       // same user+orderId => cancel
        auto c = drain(fb);
        CHECK(c.size() == 1 && c[0].orderId1 == 101 && c[0].orderId2 == engine_constants::MAX_64,
              "Cancel should emit a single-sided stub for 101");
        e.submit(2, 201, 100 * SCALE, 10, true);
        CHECK(drain(fb).empty(), "Canceled sell must not fill");
        std::cout << "  [PASS] Scenario 4: Cancel\n";
    }

    std::cout << "--------------------------------------------------\n";
    std::cout << (g_failures == 0 ? "All Logic Tests Passed.\n"
                                  : std::to_string(g_failures) + " CHECK(s) FAILED.\n");
    std::cout << "--------------------------------------------------\n";
}

struct Spec { std::uint64_t id; std::int64_t price; std::uint32_t qty; bool isBuy; };

// Pass 1 = clean wall-clock throughput; Pass 2 = per-order readTsc latency histogram.
static void reportFlow(const char* label, const std::vector<Spec>& flow,
                       std::uint64_t warmStart, std::uint64_t warmCount, double nsPerCycle) {
    const double nOrders = static_cast<double>(flow.size());

    double seconds, nanos; std::size_t trades;
    std::vector<std::uint64_t> takerCyc, makerCyc;   // derived from the engine's logged FilledStub timestamps
    {   // Pass 1: throughput (no per-order timers) + harvest logged timestamps
        FillBuf fb; Engine e {fb};
        for (std::uint64_t i {warmStart}; i < warmStart + warmCount; ++i) e.submit(1, i, 100 * SCALE, 1, true);
        const auto t0 = std::chrono::steady_clock::now();
        for (const Spec& s : flow) e.submit(s.id, s.id, s.price, s.qty, s.isBuy);
        const auto t1 = std::chrono::steady_clock::now();
        seconds = std::chrono::duration<double>(t1 - t0).count();
        nanos   = std::chrono::duration<double, std::nano>(t1 - t0).count();

        // Same data the logging thread consumes: latency = (filledTimestamp - order*Timestamp) / freq.
        const std::vector<FilledStub> fills = drain(fb);
        trades = fills.size();
        takerCyc.reserve(fills.size());
        makerCyc.reserve(fills.size());
        for (const FilledStub& f : fills) {
            if (f.orderId2 == engine_constants::MAX_64) continue;            // skip single-sided cancel stubs
            if (f.filledTimestamp >= f.order2Timestamp) takerCyc.push_back(f.filledTimestamp - f.order2Timestamp);
            if (f.filledTimestamp >= f.order1Timestamp) makerCyc.push_back(f.filledTimestamp - f.order1Timestamp);
        }
    }

    std::vector<std::uint64_t> cyc; cyc.reserve(flow.size());
    {   // Pass 2: per-submit latency, measured EXTERNALLY (fresh engine, identical flow)
        FillBuf fb; Engine e {fb};
        for (std::uint64_t i {warmStart}; i < warmStart + warmCount; ++i) e.submit(1, i, 100 * SCALE, 1, true);
        for (const Spec& s : flow) {
            const std::uint64_t a = intrinsic_funcs::readTsc();
            e.submit(s.id, s.id, s.price, s.qty, s.isBuy);
            const std::uint64_t b = intrinsic_funcs::readTsc();
            cyc.push_back(b - a);
        }
        drain(fb);
    }
    const LatStats ext = percentiles(cyc, nsPerCycle);

    std::cout << "--------------------------------------------------\n";
    std::cout << label << "\n";
    std::printf("Time Elapsed:  %.4fs\n", seconds);
    std::printf("Throughput:    %.0f Orders/Sec\n", nOrders / seconds);
    std::printf("Latency (Avg): %.2f ns\n", nanos / nOrders);
    std::printf("  per-submit  (external readTsc)       p50/p90/p99/p99.9/max: %.1f / %.1f / %.1f / %.1f / %.1f ns\n",
                ext.p50, ext.p90, ext.p99, ext.p999, ext.maxNs);
    if (!takerCyc.empty()) {
        const LatStats t = percentiles(takerCyc, nsPerCycle);
        std::printf("  taker latency   (logged entry->fill) p50/p99/p99.9/max: %.1f / %.1f / %.1f / %.1f ns\n",
                    t.p50, t.p99, t.p999, t.maxNs);
    }
    if (!makerCyc.empty()) {
        const LatStats m = percentiles(makerCyc, nsPerCycle);
        std::printf("  maker residency (logged rest->fill)  p50/p99/p99.9/max: %.1f / %.1f / %.1f / %.1f ns\n",
                    m.p50, m.p99, m.p999, m.maxNs);
    }
    std::printf("Total Trades:  %zu  (logged-latency samples: %zu)\n", trades, takerCyc.size());
    std::cout << "--------------------------------------------------\n";
}

void runBenchmark(double nsPerCycle) {
    std::cout << "Preparing benchmark data (Linear Ping-Pong)...\n";
    constexpr std::uint64_t nOrders = 1'000'000;
    std::vector<Spec> flow; flow.reserve(nOrders);
    for (std::uint64_t i {1}; i < nOrders; ++i) flow.push_back({i, 100 * SCALE, 1, (i % 2 == 0)});
    reportFlow("Type:          LINEAR PING-PONG (Ideal)", flow, nOrders + 100, 10'000, nsPerCycle);
}

void runStressTest(double nsPerCycle) {
    std::cout << "Preparing stress test data (Random Walk)...\n";
    constexpr std::uint64_t nOrders = 1'000'000;
    std::mt19937_64 rng {42};                          // fixed seed -> deterministic, reproducible workload
    std::uniform_int_distribution<int> priceDist {50, 149};
    std::uniform_int_distribution<int> sideDist {0, 1};
    std::vector<Spec> flow; flow.reserve(nOrders);
    for (std::uint64_t i {1}; i < nOrders; ++i)
        flow.push_back({i, static_cast<std::int64_t>(priceDist(rng)) * SCALE, 1, sideDist(rng) == 1});
    reportFlow("Type:          STRESS TEST (Random Walk)", flow, nOrders, 10'000, nsPerCycle);
}

int main() {
    const bool pinned = pinToCore(1);
    std::cout << (pinned ? "Pinned to core 1.\n"
                         : "Core pinning unavailable (non-Linux or denied) — running unpinned.\n");
    const double nsPerCycle = calibrateNsPerCycle();
    std::printf("readTsc resolution: %.3f ns/tick (latency floor; sub-tick orders read as 0).\n\n", nsPerCycle);

    runCorrectnessTest();
    runBenchmark(nsPerCycle);
    runStressTest(nsPerCycle);
    return g_failures == 0 ? 0 : 1;
}
