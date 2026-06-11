# orderbeamer — testing suite

Three layers, from narrowest/fastest to most realistic. Caution: most of these measure the matcher *in isolation*, not the whole system. (Disclaimer: benchmarks/tests were created with the assistance of generative AI.)

| Test | Drives | End-to-end? | Platform |
| :--- | :--- | :--- | :--- |
| `bench.cpp`  | matching core, in-process | **No** — bypasses gateway/threads | macOS or Linux |
| `stress.cpp` | matching core vs a reference, in-process | **No** — bypasses gateway/threads | macOS or Linux |
| `e2e_client.py` + `run_e2e.sh` | the real binary over the UDS gateway | **Yes** | Linux only (epoll/UDS) |

> **What "in-process" means here:** `bench` and `stress` include the engine headers and call the
> matching functions directly through a thin `Engine` wrapper that replicates `matchingFunc()`'s body.
> They do **not** go through the `AF_UNIX` socket, the gateway thread, the SPSC ring, or thread pinning —
> so their nanosecond numbers are the *matcher alone*, with zero transport cost. Only `run_e2e.sh`
> exercises the full `Python → socket → gateway → SPSC → matcher → logging` path.

---

## Running

### In-process (works on macOS too)
```bash
# from the project root
clang++ -O3 -std=c++20 -I. -o /tmp/bench tests/bench.cpp && /tmp/bench
clang++ -O3 -std=c++20 -I. -o /tmp/stress tests/stress.cpp && /tmp/stress
```
Or via CMake (the `bench` / `stress` targets, both forced to `-O3`). Under the CLion **Docker toolchain**, they build/run in Linux and self-pin to a core (part of achieving optimizations).

### End-to-end (Linux only)
```bash
docker run --rm -v "$PWD":/app -w /app clion-ubuntu24:latest bash tests/run_e2e.sh 200000
```
`run_e2e.sh` builds the engine, starts it, waits for the gateway to bind `/tmp/orderbeamer.sock`,
feeds it `N` orders with `e2e_client.py`, then reports.

---

## What each test does

### `bench.cpp` — correctness + speed of the matcher (in-process)
- **Correctness (4 scenarios):** basic fill, price priority, time priority (FIFO), and a cancel.
  Asserts on the drained `FilledStub`s (and that the logged timestamps are set + ordered).
- **Throughput:** "Linear ping-pong" (1M alternating buy/sell at one level — the ideal case) and
  "Random walk" (1M uniform-random price/side — realistic).
- **Latency, two views:**
  - *per-submit* — an external `readTsc` bracket around each `submit()`;
  - *logged* — derived from the engine's own `FilledStub` timestamps (taker entry→fill, maker rest→fill),
    i.e. the exact data the logging thread consumes.
- On Apple Silicon the `readTsc` tick is ~42 ns, so sub-42 ns percentiles read as 0 (the floor).

### `stress.cpp` — differential correctness under chaos (in-process)
- Generates a **cancel-heavy (~45%), mixed-quantity (1–40), wide-price (1025 levels), 8-user** op-stream
  and runs it through **both** the orderbeamer core and a dead-simple `std::map` reference book,
  asserting the fill streams match **op-for-op**. Any divergence prints a concrete repro.

### `e2e_client.py` + `run_e2e.sh` — the full pipe (end-to-end, Linux)
- `e2e_client.py` connects over `AF_UNIX` / `SOCK_SEQPACKET`, packs the 64-byte `Order` wire layout,
  and sends a ping-pong stream, reporting the client send rate.
- `run_e2e.sh` wires the engine + client together and reports fills logged + sample matching latency.
- This is the only test that measures the **gateway + SPSC + thread** overhead on top of the matcher.

---

## Sample metrics (representative single runs)

Numbers vary run-to-run, especially inside the Docker VM. These are typical
values observed during development.

### `bench.cpp` — matcher only, in-process
Correctness: **4 / 4 scenarios pass.**

| Workload | Native macOS (M-series, `-O3 -mcpu=native`) | Docker arm64 Linux (pinned, `-O3`) |
| :--- | :--- | :--- |
| Linear ping-pong | ~66 M ops/sec · ~15 ns avg | ~46 M ops/sec · ~21 ns avg |
| Random walk      | ~39 M ops/sec · ~26 ns avg | ~44 M ops/sec · ~23 ns avg |

Latency (Docker, ping-pong): per-submit p50/p90/p99/p99.9 = `0 / 41.7 / 41.7 / 41.7 ns`;
logged taker (entry→fill) p99.9 = `41.7 ns`. Maker residency p50 ≈ `417 µs` — a *workload* artifact
(the warm-up leaves a deep FIFO queue), and throughput is unaffected by it, i.e. O(1)-in-depth.
Random walk: taker p99.9 ≈ `83 ns`; maker residency p50 ≈ `208 ns` out to a ~20 ms tail.

### `stress.cpp` — differential vs `std::map` reference, in-process
1,000,000 ops · 99,326 real cancels · 684,018 fills · **0 divergences (PASS).**
Throughput on that cancel-heavy / mixed-qty / wide-price workload (macOS, submit-only):
**22.0 M ops/sec, ~45 ns/op** — up from 6.2 M / 161 ns before the O(1) cancel fix.
The index-0 boundary level is exercised by the differential and matches the reference (0 divergences).

### End-to-end — full pipe, Docker (Linux)
200,000 ping-pong orders → **199,969 fills logged** (the ~31 missing are the unflushed `ofstream` tail).

| Sender | Throughput | Notes |
| :--- | :--- | :--- |
| `e2e_client.py` (lean)      | ~0.82 M orders/sec (~1.2 µs/order) | Python send-bound |
| `client.py` (Client class)  | ~0.38 M orders/sec (~2.6 µs/order) | + id-gen, `Decimal` price math, asserts |

The matcher's internal fill latency bottoms at the ~42 ns floor and it sits **idle ~96 % of the time**
waiting on the Python sender — the gateway/SPSC is nearly free; the bottleneck is the client, not the engine.
