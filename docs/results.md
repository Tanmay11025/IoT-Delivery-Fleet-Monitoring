# Results and Conclusions

## 1. Executive Summary (Latest Run — 2026-10-18)

All test categories passed on this run. Detailed environment, metric definitions, per-worker vectors, stage data, and analytical notes are in [tests.md](tests.md).

| Test category | Outcome |
| --- | --- |
| C++ CTest (31 cases) | **31/31 passed** |
| Docker HTTP integration (13 cases) | **13/13 passed** |
| Staged HTTP concurrency — 9 stages | **9/9 passed** |
| Graceful shutdown under load | **9/9 clean exits** |
| Persistence recovery across restart | **9/9 exact acknowledged-record sets recovered** |

---

## 2. Build and Unit-Test Results

### 2.1 Full suite

The full 31-case C++ CTest suite was run with:

```bash
cmake -S . -B build
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Result:

```text
100% tests passed, 0 tests failed out of 31
Total Test time (real) = 0.23 sec
```

### 2.2 Covered behaviors

| Test target | Behaviors verified |
| --- | --- |
| `http_parser_tests` | Fragmented headers and bodies; `Content-Length` completion; requests without bodies; case-insensitive header lookup; query/path separation; malformed requests; conflicting lengths; unsupported chunked encoding; header/body size limits |
| `rate_limiter_tests` | Fake-clock token-bucket burst/refill/cap; zero-rate behavior; independent IP buckets; `429` handler bypass; concurrent same-IP consumption capped at capacity |
| `metrics_tests` | Connection lifecycle; request/blocked counts; response classes; duration histogram; parse errors; process uptime; log disk-byte gauge |
| `config_tests` | Config-file parsing; CLI override precedence; invalid-value rejection; unknown-key rejection |
| `thread_pool_tests` | Fixed worker stats; bounded-queue rejection; rejection after shutdown starts; draining of accepted tasks; continued worker operation after a task throws |
| `tcp_server_drain_tests` | Three messages appended through API; chronological ordering in `/recent`; `count=1` selects only the newest; JSON escaping; `count=0` and `count=1001` boundary validation; `gateway_log_disk_bytes` in `/metrics`; in-flight handler released after SIGTERM; response delivered before server thread joins; `gateway_thread_pool_workers` present in post-shutdown exposition |
| `mmap_log_tests` | Append/read across close and reopen; variable and empty payloads; global offsets across segment rollover; exact-fit rollover; oversized-record rejection; incomplete-header/payload recovery; payload bytes before a missing header; active-tail checksum rejection; fatal corruption in a sealed segment |
| `circular_buffer_tests` | Chronological reads before and after wraparound; exact-capacity fills; zero/oversized reads; capacity-one replacement |

---

## 3. HTTP Integration Test Results

Run with `./scripts/run_http_integration_test.sh`. Docker Compose starts the compiled gateway and a Python `python:3.12-alpine` test container. The gateway's rate-limit override (`--rate-limit-capacity 1000 --rate-limit-refill-rate 0`) prevents the shared container IP from exhausting the token bucket.

**Result: 13/13 passed.**

```text
PASS test_health_request
PASS test_byte_by_byte_request
PASS test_fragmented_body
PASS test_malformed_requests
PASS test_header_normalization_and_repeated_headers
PASS test_conflicting_content_length
PASS test_size_limits
PASS test_routing
PASS test_concurrent_clients
PASS test_slow_reader_partial_response
PASS test_recent_endpoint
PASS test_metrics_endpoint
PASS test_publish_endpoint
PASS all HTTP integration tests (13)
http-test exited with code 0
```

The gateway's exit code `137` during Compose teardown is expected (`SIGKILL` from `--abort-on-container-exit`) and is not a test failure.

### 3.1 What the results demonstrate

- The parser assembles requests delivered in tiny TCP fragments.
- The parser does not route before headers and the declared body are complete.
- Invalid framing and unsupported protocol features (`Transfer-Encoding`, conflicting lengths) are rejected.
- Header names are normalized; repeated headers follow the implemented rules.
- Header and body limits are enforced.
- Exact routing returns the expected success or `404` response.
- Multiple clients can use the server concurrently without response mixing.
- A slow reader receives a complete 512 KiB response through the outbound queue and `EPOLLOUT` path.
- `/metrics` returns Prometheus text with the expected content type and all required samples; request counters and histogram counts advance correctly across scrapes.
- `/recent` returns an empty array on a fresh server, validates count parameters, and returns the correct chronological set.
- `/publish` rejects an empty body with `400`; a non-empty body returns `201 Created` and the record is immediately visible through `/recent`.

### 3.2 Troubleshooting note — rate limiter vs. integration tests

The first HTTP integration run after adding per-IP rate limiting failed: all test clients shared the same Docker container IP, so earlier tests consumed the default 10-token bucket. Subsequent tests received `429 Too Many Requests`. Binding clients to different loopback addresses was not portable inside the Alpine network namespace. The final fix was to configure the integration Compose service with a large capacity and zero refill rate. This preserves the production default while making transport-layer tests independent of the rate-limiting policy.

---

## 4. Staged HTTP Concurrency Results

Run with `python3 scripts/run_backend_tests.py`. The runner sequenced all nine stages. Each stage opened N connections, sent `POST /publish` to N-1 clients and `GET /test/hold` to one, then triggered shutdown and verified recovery.

### 4.1 Summary table

| Concurrent clients | Connected | Acknowledged / recovered writes | Avg publish latency | P95 publish latency | Offered req/s | Shutdown |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 100 | 100 | 99 / 99 | 2.71 ms | 3.23 ms | 82,520 | Passed |
| 500 | 500 | 499 / 499 | 19.84 ms | 22.77 ms | 29,644 | Passed |
| 1,000 | 1,000 | 999 / 999 | 31.68 ms | 34.62 ms | 43,894 | Passed |
| 2,500 | 2,500 | 2,499 / 2,499 | 65.38 ms | 68.99 ms | 62,879 | Passed |
| 5,000 | 5,000 | 4,999 / 4,999 | 133.58 ms | 140.51 ms | 62,789 | Passed |
| 10,000 | 10,000 | 9,999 / 9,999 | 229.27 ms | 248.12 ms | 77,814 | Passed |
| 20,000 | 20,000 | 19,999 / 19,999 | 437.06 ms | 477.75 ms | 90,154 | Passed |
| 30,000 | 30,000 | 29,999 / 29,999 | 700.27 ms | 753.07 ms | 87,177 | Passed |
| 50,000 | 50,000 | 49,999 / 49,999 | 1.605 s | 1.704 s | 53,386 | Passed |

At every stage: zero connect, send, and response-read failures; no `429` or `503` responses; no parse errors.

### 4.2 Resource usage at 50,000 clients

| Metric | Value |
| --- | --- |
| Peak CPU (observed) | 26.55% |
| Peak RSS (observed) | 212.3 MiB |
| Pool queue high-water | 88 |
| Max per-worker completion queue high-water | 42 |
| Connection-establishment time | 76.4 s |

### 4.3 Key observations

- **Connection scaling**: `SO_REUSEPORT` distributed 50,000 clients across 8 workers with coefficient of variation < 1% (6,173–6,347 per worker).
- **Latency scaling**: average publish latency grew from 2.71 ms at 100 clients to 1.605 s at 50,000, consistent with a burst model where all requests arrive nearly simultaneously.
- **Shutdown drain**: all 9 stages exited cleanly within 11.2–13.3 s. The deliberate held handler always completed before the 120-second compose-stop timeout.
- **Persistence**: 100% of acknowledged writes were recovered at every stage. The restarted gateway reached ready state at all 9 stages.
- **In-flight at shutdown**: only 1 request was in flight at SIGTERM time at every stage (the deliberate hold). Bulk writes had all received their `201 Created` responses before SIGTERM.

---

## 5. Connection-Ramp Test (Historical)

This test used the raw TCP echo generator (`scripts/tcp_load_generator.py`) and preceded the HTTP backend test. It verified TCP connection retention, not HTTP request handling.

| Target connections | Hold time | Result | Elapsed |
| ---: | ---: | --- | ---: |
| 1,000 | 30 s | 1,000 successful, 0 failures | 30.14 s |
| 5,000 | — | Successful, 0 failures | Not recorded |
| 10,000 | 60 s | 10,000 successful, 0 failures | 60.97 s |
| 25,000 | 90 s | 25,000 successful, 0 failures | 98.97 s |
| 50,000 (first) | 120 s | 28,230 successful, 21,770 failures | 39.24 s |
| 50,000 (corrected) | 120 s | 50,000 successful, 0 failures | 153.99 s |

The first 50,000-client attempt failed with ~21,770 failures consistent with ephemeral source-port exhaustion on the single load-generator container. Expanding the source port range to 1024–65535 resolved this. The lesson: high-connection tests must configure both sides.

> [!NOTE]
> The raw TCP test is a connection-retention check, not an HTTP capacity measurement. Results from that test should not be used to infer HTTP throughput.

---

## 6. Conclusions

### 6.1 What the results demonstrate

| Capability | Evidence |
| --- | --- |
| Correct TCP fragmentation handling | HTTP integration test: byte-by-byte and fragmented-body cases |
| Correct backpressure and partial-write path | HTTP integration test: slow-reader 512 KiB case |
| Request concurrency without mixing | HTTP integration test: 100 concurrent raw-socket clients |
| Security policy isolation | Rate-limiter unit tests + integration suite rate-limit override |
| Configuration correctness | Config unit tests: file/CLI precedence, invalid-value rejection |
| Pool queue bounding and task draining | Thread-pool unit tests: queue rejection, shutdown drain |
| Graceful shutdown under in-flight work | `tcp_server_drain_tests` + staged backend test (9 stages) |
| Persistent log correctness | `mmap_log_tests` (8 cases covering recovery, corruption, rollover) |
| Cache consistency | `circular_buffer_tests` (4 cases) |
| HTTP scalability to 50,000 clients | Staged backend test: 9/9 stages, zero failures |
| Persistence durability across restart | Staged backend test: 9/9 recovery verifications |
| Worker load distribution | Per-worker metrics (CV < 1% at 50,000 clients) |

### 6.2 What the results do not demonstrate

- **Sustained-rate HTTP throughput**: the test is a burst-capacity check; all requests arrive simultaneously after connections are open. Results are not a sustained req/s benchmark.
- **Many simultaneous blocked handlers**: only one handler was in flight at SIGTERM. Shutdown drain under thousands of blocked application handlers was not tested.
- **Power-loss durability**: the test uses graceful process shutdown and segment-file recovery. Machine-level crash recovery was not verified.
- **Multi-host or production network**: all measurements are on a single-host Docker bridge network.
- **Telemetry JSON validation**: `POST /publish` accepts opaque non-empty bodies. JSON field validation and downstream delivery are not implemented.
- **Authentication and sustained rate-limit effectiveness**: token-bucket correctness is verified by unit tests; a sustained security-load benchmark under hostile traffic was not run.
- **Inactive-IP bucket expiration**: the limiter map grows without bound; high-cardinality IP traffic was not tested.

### 6.3 Overall assessment

The gateway has a proven event-driven transport foundation: it accepts, serves, and cleans up tens of thousands of simultaneous HTTP clients, shuts down gracefully while in-flight work completes, and recovers all acknowledged writes from persistent storage across process restart. The next steps — telemetry JSON validation, downstream delivery, authentication, and consumer streaming — build on this validated foundation.
