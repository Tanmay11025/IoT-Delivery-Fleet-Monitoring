# IoT Delivery Fleet Monitoring

[![CI](https://github.com/Tanmay11025/IoT-Delivery-Fleet-Monitoring/actions/workflows/ci.yml/badge.svg)](https://github.com/Tanmay11025/IoT-Delivery-Fleet-Monitoring/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Linux](https://img.shields.io/badge/platform-Linux-lightgrey)

A live telemetry platform for delivery vans, built from scratch in C++17 on
Linux. Vans publish position and status over HTTP. The gateway validates each
report and appends it to a crash-safe log. A built-in dashboard shows the
fleet on a live map, and downstream services replay or stream every report
over a binary protocol, resuming exactly where they stopped after a crash.

In design terms, it is a small combination of **Nginx** (an epoll event
loop per core in front of a worker pool), **Kafka** (a segmented, offset-indexed
append-only log with replay and at-least-once consumers) and **Prometheus**
(a `/metrics` endpoint plus a dashboard).

![Fleet Ops dashboard](docs/images/dashboard.png)

<!-- To refresh: run the Quick start below, then capture the browser window. -->

## Highlights

- **~77,000 validated telemetry events/s** at 1.03 ms P99
  (120-byte events, 100 connections, 4 pool threads, one laptop) ·
  [benchmark report](docs/performance_results.md)
- **50,000 concurrent clients** in the staged burst test, with every
  acknowledged write recovered after restart · [results](docs/results.md)
- **Acknowledged writes survive `kill -9`** mid-stream, verified by a chaos
  test that crashes the gateway while four publishers are sending
- **Consumers never skip a record.** They resume from a saved offset after
  either side crashes (at-least-once delivery, tested both ways)
- **A measured optimisation:** callgrind found a bit-at-a-time CRC32 inside
  the log lock. A table-driven version made 8 KiB publishes **3.2× faster**
  · [journal](docs/journal.md)

## Architecture

```mermaid
flowchart LR
    V["Vehicles<br/>fleet_simulator.py"] -- "HTTP POST /publish" --> GW
    D["Fleet Ops Dashboard"] -- "GET /metrics /vehicles /recent" --> GW
    C["Consumers (Python SDK)"] <-- "binary TCP :9090<br/>FETCH_FROM → DATA, ACK" --> GW
    subgraph GW["Gateway (C++17)"]
        direction TB
        EP["epoll worker per core<br/>SO_REUSEPORT"] --> RL["per-IP token bucket"] --> TP["bounded worker pool"]
        TP --> VAL["TelemetryEvent validation"] --> LOG[("segmented mmap log<br/>CRC per record")]
        VAL --> FS["FleetState"]
        LOG --> RC["recent cache"]
    end
```

More detail: [architecture.md](docs/architecture.md) (lifecycle and
concurrency diagrams) · [protocol.md](docs/protocol.md) (HTTP API, telemetry
schema, binary frames byte by byte).

| Area | What is in the code | Where |
| --- | --- | --- |
| Operating systems | `epoll`, non-blocking sockets, `eventfd`, `mmap`/`msync`/`ftruncate`, signals | `src/network`, `src/storage` |
| Networking | TCP fragmentation handling, incremental HTTP parser, length-prefixed binary protocol, network byte order | `src/http`, `src/protocol` |
| Concurrency | thread pool + condition variable, `shared_mutex`, per-worker ownership with no locks on the socket path | `src/network/thread_pool.h`, `src/telemetry/fleet_state.cpp` |
| Data structures | token bucket, ring buffer, hash maps, binary search over the segment index | `src/security`, `src/storage` |
| Storage and reliability | append-only segments, CRC32, torn-tail recovery, offset replay, at-least-once consumers | `src/storage/mmap_log.cpp` |
| Testing | 51 C++ tests, 35 pytest integration/chaos tests, 13 Docker HTTP tests, `wrk` benchmarks, ASan/UBSan in CI | `tests/` |

## Quick start (Ubuntu)

```bash
sudo apt install build-essential cmake python3
cmake -S . -B build && cmake --build build -j"$(nproc)"

# Terminal 1: gateway with demo-sized rate limits (all simulated vans share one IP)
./build/message_gateway --config config.demo.ini

# Terminal 2: 40 simulated vans driving around central London
python3 examples/fleet_simulator.py --vehicles 40

# Browser: the dashboard
xdg-open http://localhost:8080/

# Terminal 3: a dispatch monitor that alerts on speeding or silent vans and
# resumes from its offset file after a restart
python3 examples/fleet_consumer.py --speed-limit 70 --offset-file dispatch.offset

# Afterwards: where was one van, and when?
python3 examples/replay_vehicle.py --vehicle van-00003
```

`config.example.ini` keeps the strict defaults (10-token bucket, 5 tokens/s
per IP). Every setting can also be overridden with a CLI flag
(`./build/message_gateway --help` prints them).

## Python SDK

```bash
pip install -e python_sdk      # standard library only
```

```python
from gateway_client import GatewayClient

client = GatewayClient("127.0.0.1", http_port=8080, consumer_port=9090,
                       offset_file="dispatch.offset")

client.send_telemetry({"vehicle_id": "van-00482", "lat": 51.5074, "lng": -0.1278,
                       "speed": 32.5, "status": "moving", "timestamp": 1726310400})

def handle(message):                     # called once per record, in log order
    print(message.offset, message.json()["vehicle_id"])

client.subscribe_stream(handle)          # resumes from dispatch.offset if present
```

## Reliability guarantees, stated precisely

| Guarantee | Evidence |
| --- | --- |
| A `201` publish survives a crash of the gateway **process** (`kill -9`) | `tests/chaos/test_kill9.py` |
| SIGTERM drains in-flight requests, exits 0, loses nothing acknowledged | `tests/chaos/test_graceful_shutdown.py`, staged test (9/9 stages) |
| Torn or corrupt records at the log tail are detected (CRC32) and truncated on restart | `mmap_log_tests` |
| A consumer never skips a record after a consumer or gateway crash; it may see one twice | `tests/integration/test_consumer_reconnect.py` |

**Not guaranteed:** surviving power loss or a kernel crash. Records reach the
page cache, but they are not `fsync`'d one by one; segments are synced when
full and at shutdown. This is a deliberate throughput trade-off (see
[protocol.md §3](docs/protocol.md#3-durability-boundary)).

## Testing

```bash
ctest --test-dir build                       # 51 C++ unit tests
pip install pytest && python3 -m pytest     # 35 integration + chaos tests (starts real gateways)
bash scripts/run_http_integration_test.sh    # 13 raw-socket HTTP tests in Docker
python3 scripts/run_backend_tests.py         # staged 100 → 50,000 client burst test
tests/benchmarks/run_bench.sh                # wrk throughput matrix
```

CI runs the C++ and pytest suites in Release, again under
AddressSanitizer + UBSan, the chaos tests, and the Docker HTTP suite.

## Repository layout

```text
src/
  network/     TCPServer, EpollLoop, ConnectionManager, ThreadPool, consumer pump
  http/        HTTPParser, HttpResponse, Router, dashboard static files
  protocol/    binary consumer frame codec
  telemetry/   TelemetryEvent validation, FleetState
  storage/     MemoryMappedLog, CircularBuffer
  security/    TokenBucket, RateLimiter
  monitoring/  Prometheus metrics
dashboard/     Fleet Ops dashboard (HTML/CSS/vanilla JS + Chart.js)
python_sdk/    gateway_client package
examples/      fleet_simulator.py, fleet_consumer.py, replay_vehicle.py
tests/         C++ unit tests, integration/, chaos/, benchmarks/
docs/          architecture, protocol, performance results, journal, test reports
```

## Known limitations and future work

- **Security:** no TLS or authentication. The per-IP rate limiter is the only
  edge control, and its IP map is not yet bounded or expired.
- **Single node:** no replication, partitioning, or consumer groups. Multiple
  gateways would need shared rate-limit state and a partitioned log.
- **Delivery semantics:** at-least-once, not exactly-once, which would need
  the consumer's offset commit and side effects to be atomic.
- **Rollover stall:** a full segment is `msync`'d while appends wait, which
  shows up as P99 spikes with large bodies. The fix is to sync sealed
  segments in the background ([performance results](docs/performance_results.md)).
- **HTTP:** one request per connection (no keep-alive); chunked encoding is
  rejected.
- **Fleet features:** geofencing rules, route history queries by time (replay
  is by offset today), and multi-region fleets.

## Documentation

[architecture.md](docs/architecture.md) ·
[protocol.md](docs/protocol.md) ·
[performance_results.md](docs/performance_results.md) ·
[journal.md](docs/journal.md) ·
[results.md](docs/results.md) / [tests.md](docs/tests.md) (staged load tests) ·
[implementation.md](docs/implementation.md) ·
[project overview.md](docs/project%20overview.md)
