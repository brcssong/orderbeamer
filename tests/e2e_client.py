#!/usr/bin/env python3
"""
End-to-end test client.

Unlike tests/bench.cpp and tests/stress.cpp (which drive the matching core IN-PROCESS,
bypassing the network), this exercises the REAL path:

    Python  -->  AF_UNIX / SOCK_SEQPACKET socket  -->  gateway thread (epoll)
            -->  SPSC ring buffer  -->  matching thread  -->  logging thread -> file

It measures how fast the full pipe ingests orders. Run it against a live `orderbeamer`
binary (see tests/run_e2e.sh). Usage: e2e_client.py [n_orders]
"""
import socket
import struct
import sys
import time

SOCK_PATH = "/tmp/orderbeamer.sock"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 200_000

# 64-byte wire layout, matching `struct alignas(64) Order` in engine/statics.h:
#   u64 timestamp, u64 trueTimestamp, u64 userId, u64 orderId, i64 price,
#   u32 qty, u32 prev, u32 next, u32 priceLevelArenaIdx, bool isBuy, + pad to 64.
_BASE = "@QQQQqIIII?"
_FMT = _BASE + str(64 - struct.calcsize(_BASE)) + "x"
_MAX64 = 0xFFFFFFFFFFFFFFFF
_MAX32 = 0xFFFFFFFF
_PRICE = 10 ** 9          # client convention price*1e9; engine index = price/1e7 -> level 100


def pack(user_id: int, order_id: int, is_buy: bool) -> bytes:
    # timestamp is filled in by the engine; trueTimestamp = client send time (carried, not matched on).
    return struct.pack(_FMT, _MAX64, time.monotonic_ns(), user_id, order_id,
                       _PRICE, 1, _MAX32, _MAX32, _MAX32, is_buy)


def main() -> int:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    for _ in range(200):                       # wait for the gateway to bind the socket
        try:
            s.connect(SOCK_PATH)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.05)
    else:
        print("ERROR: gateway socket never appeared at " + SOCK_PATH, file=sys.stderr)
        return 1

    # Ping-pong: alternate buy/sell at one price level, two users (so it is never a wash),
    # unique order ids (so nothing is misread as a cancel). Every pair crosses -> a fill.
    t0 = time.perf_counter()
    for i in range(1, N + 1):
        is_buy = (i % 2 == 0)
        s.send(pack(2 if is_buy else 1, i, is_buy))
    t1 = time.perf_counter()
    s.close()

    dt = t1 - t0
    print(f"client sent {N} orders in {dt:.4f}s  ->  "
          f"{N / dt / 1e6:.3f} M orders/sec  ({dt / N * 1e9:.0f} ns/order, Python send-bound)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
