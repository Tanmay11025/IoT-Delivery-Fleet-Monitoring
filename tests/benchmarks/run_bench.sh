#!/usr/bin/env bash
# Sustained-throughput benchmark for POST /publish.
#
# Matrix: request-pool threads x body size x concurrent connections. Each cell
# starts a fresh gateway on an empty log, warms up for 2 s, then runs wrk for
# DURATION seconds, REPEATS times. The gateway and wrk are pinned to separate
# CPU sets so the load generator does not steal the server's cores.
#
#   tests/benchmarks/run_bench.sh            # full matrix, 8 s x 3 repeats
#   DURATION=5 REPEATS=1 POOLS="4" tests/benchmarks/run_bench.sh
#
# Output: tests/benchmarks/results/bench-<timestamp>.csv, summarised by
# tests/benchmarks/summarize.py.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BINARY="${GATEWAY_BINARY:-$ROOT/build/message_gateway}"
SCRIPT="$ROOT/tests/benchmarks/telemetry.lua"
DURATION="${DURATION:-8}"
REPEATS="${REPEATS:-3}"
POOLS="${POOLS:-2 4 8}"
PAYLOADS="${PAYLOADS:-120 1024 8192}"
CONNECTIONS="${CONNECTIONS:-100 1000}"
EPOLL_WORKERS="${EPOLL_WORKERS:-4}"
SERVER_CPUS="${SERVER_CPUS:-0-7}"
CLIENT_CPUS="${CLIENT_CPUS:-8-15}"
WRK_THREADS="${WRK_THREADS:-4}"
PORT="${PORT:-18181}"
SEGMENT_SIZE="${SEGMENT_SIZE:-268435456}"
RESULTS="$ROOT/tests/benchmarks/results"
mkdir -p "$RESULTS"
CSV="$RESULTS/bench-$(date +%Y%m%d-%H%M%S).csv"

command -v wrk >/dev/null || { echo "wrk is required (apt install wrk)"; exit 1; }
[[ -x "$BINARY" ]] || { echo "build the gateway first: $BINARY"; exit 1; }
ulimit -n 65536 2>/dev/null || true

# Convert wrk latency strings (850.12us, 1.20ms, 2.01s) to milliseconds.
to_ms() {
  awk -v value="$1" 'BEGIN {
    if (value ~ /us$/) { sub(/us$/, "", value); printf "%.3f", value / 1000 }
    else if (value ~ /ms$/) { sub(/ms$/, "", value); printf "%.3f", value }
    else if (value ~ /s$/) { sub(/s$/, "", value); printf "%.3f", value * 1000 }
    else printf "nan" }'
}

GATEWAY_PID=""
STORAGE=""
cleanup() {
  [[ -n "$GATEWAY_PID" ]] && kill -INT "$GATEWAY_PID" 2>/dev/null && wait "$GATEWAY_PID" 2>/dev/null || true
  [[ -n "$STORAGE" ]] && rm -rf "$STORAGE"
  GATEWAY_PID=""; STORAGE=""
}
trap cleanup EXIT

start_gateway() {
  STORAGE="$(mktemp -d)"
  taskset -c "$SERVER_CPUS" "$BINARY" --port "$PORT" --consumer-port 0 \
    --epoll-workers "$EPOLL_WORKERS" --pool-threads "$1" --queue-capacity 4096 \
    --backlog 4096 --rate-limit-capacity 1000000000 --rate-limit-refill-rate 1000000000 \
    --storage-directory "$STORAGE" --storage-segment-size "$SEGMENT_SIZE" \
    --dashboard-directory "$ROOT/dashboard" >/dev/null 2>&1 &
  GATEWAY_PID=$!
  for _ in $(seq 100); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && return 0
    sleep 0.05
  done
  echo "gateway did not start"; exit 1
}

echo "pool_threads,payload_bytes,connections,run,rps,p50_ms,p90_ms,p99_ms,non_2xx,socket_errors" > "$CSV"
for pool in $POOLS; do
  for payload in $PAYLOADS; do
    for connections in $CONNECTIONS; do
      for run in $(seq "$REPEATS"); do
        start_gateway "$pool"
        url="http://127.0.0.1:$PORT/publish"
        taskset -c "$CLIENT_CPUS" wrk -t"$WRK_THREADS" -c"$connections" -d2s -s "$SCRIPT" \
          "$url" -- "$payload" >/dev/null 2>&1 || true
        output="$(taskset -c "$CLIENT_CPUS" wrk -t"$WRK_THREADS" -c"$connections" \
          -d"${DURATION}s" --latency -s "$SCRIPT" "$url" -- "$payload")"
        rps="$(awk '/^Requests\/sec:/ {print $2}' <<<"$output")"
        p50="$(to_ms "$(awk '$1 == "50%" {print $2}' <<<"$output")")"
        p90="$(to_ms "$(awk '$1 == "90%" {print $2}' <<<"$output")")"
        p99="$(to_ms "$(awk '$1 == "99%" {print $2}' <<<"$output")")"
        non2xx="$(awk '/Non-2xx/ {print $NF}' <<<"$output")"
        socket_errors="$(awk '/Socket errors/ {gsub(",", ""); print $4 + $6 + $8 + $10}' <<<"$output")"
        line="$pool,$payload,$connections,$run,$rps,$p50,$p90,$p99,${non2xx:-0},${socket_errors:-0}"
        echo "$line" | tee -a "$CSV"
        cleanup
      done
    done
  done
done
echo "Results: $CSV"
python3 "$ROOT/tests/benchmarks/summarize.py" "$CSV"
