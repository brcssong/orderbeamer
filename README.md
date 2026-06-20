# orderbeamer

A low-latency limit-order-book matching engine in C++20. Orders arrive over a Unix socket, get matched against a resting book, and fills are handed to a logger. Everything on the hot path runs on pinned threads that pass work through lock-free buffers, and nothing allocates after startup.

## Data structures

The book is built from three structures, and they are most of the design:

- **PriceLevelArena** — one per side (bids, asks). Price levels are held as a hierarchical bitset over a fixed range of ticks instead of a tree or a map, so finding the best bid or ask is a couple of hardware bit-scans rather than a search. Each live level points at an intrusive doubly-linked list of the orders resting at that price, kept in time priority.

- **OrderArena** — where the orders live. Every order takes a slot in a fixed arena and is referred to by a 32-bit index instead of a pointer, so there is no per-order allocation and the linked lists above are just chains of indices.

- **OADTable** — an open-addressing hash table owned by the OrderArena that maps an order ID to its arena slot. This is what makes a cancel O(1): look up the ID, jump straight to the slot, unlink it. No walking the book.

Best bid/ask is O(1), a match walks a single price level, and a cancel is a direct hit. The price range and the arena sizes are all fixed up front.

## Threads and sockets

The engine runs as a few threads, each pinned to its own core so they do not migrate or share caches: a gateway thread that reads the socket and parses orders, a matcher that drains them and runs the book, a logger for fills, and a small error drainer per producer.

Threads hand work to each other through **Buffer**, a single-producer/single-consumer ring. Its read and write indices sit on separate cache lines so the two ends never contend on the same line, and there are no locks on the path.

Orders come in over a Unix domain socket (`AF_UNIX` / `SOCK_SEQPACKET`), read with epoll. There is no TCP — the client is assumed to be on the same machine.

## Layout

- `memory/` — the data structures (PriceLevelArena, OrderArena, Buffer, Allocator)
- `engine/` — thread setup and the matching loop
- `gateway/` — the socket and epoll layer
- `client/` — a Python client that speaks the wire format
- `tests/` — benchmarks and a differential stress test (see `tests/README.md`)

## Building

Linux only — it uses epoll, Unix sockets, and thread pinning.

```
cmake -B build && cmake --build build
```
