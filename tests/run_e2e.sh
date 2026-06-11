#!/usr/bin/env bash
# End-to-end orchestrator: build the real engine, start it, feed it through the Python
# client over the UDS gateway, then report. Linux-only (epoll/UDS) -> run inside Docker:
#   docker run --rm -v "$PWD":/app -w /app clion-ubuntu24:latest bash tests/run_e2e.sh [n]
set -u
N="${1:-200000}"
cd "$(dirname "$0")/.."

echo "building orderbeamer engine (-O3)..."
g++ -O3 -std=c++20 -I. -o /tmp/orderbeamer engine/engine.cpp || { echo "BUILD FAILED"; exit 1; }

rm -f /tmp/orderbeamer.sock completed_orders.txt
/tmp/orderbeamer >/tmp/engine.out 2>&1 &
PID=$!
for _ in $(seq 1 200); do [ -S /tmp/orderbeamer.sock ] && break; sleep 0.05; done
[ -S /tmp/orderbeamer.sock ] || { echo "engine never bound socket:"; cat /tmp/engine.out; kill -9 "$PID" 2>/dev/null; exit 1; }

echo "feeding $N orders through the gateway..."
python3 tests/e2e_client.py "$N"

sleep 2   # let the matching + logging threads drain to the file
echo "--------------------------------------------------"
echo "engine threads up: $(grep -ic 'begun running' /tmp/engine.out) / 2"
echo "fills logged (flushed) : $(wc -l < completed_orders.txt 2>/dev/null || echo 0)"
echo "sample matching latency (engine TSC, internal-only):"
grep -oE 'TIME [0-9.]+' completed_orders.txt 2>/dev/null | head -3 | sed 's/^/    /'
kill -9 "$PID" 2>/dev/null
echo "--------------------------------------------------"
