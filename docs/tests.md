# Backend Concurrency, Shutdown, and Persistence Tests

## 1. Pass Criteria

The following thresholds were set before running any load stage:

| Criterion | Threshold |
| --- | --- |
| Connection failures | 0 |
| Send failures | 0 |
| Response read failures | 0 |
| 429 Too Many Requests | 0 |
| 503 Service Unavailable | 0 |
| Parse errors | 0 |
| Graceful shutdown exit code | 0 |
| Persistence recovery | 100% of acknowledged writes |
| Max publish P95 latency at 50,000 | < 5 seconds |
| Shutdown drain duration | < 120 seconds |

Each stage must satisfy all criteria to be counted as passed.

---

## 2. Test Environment

### 2.1 Host

| Parameter | Value |
| --- | --- |
| OS | Ubuntu 24.04 |
| Kernel | 7.0.0-31-generic |
| CPU | 12th Gen Intel Core i5-1240P |
| Logical CPUs | 16 (8 P-cores × 2 threads) |
| Total RAM | 15.29 GiB (16,031,120 KiB) |
| Host `nofile` limit | 1,048,576 |
| `somaxconn` | 4,096 |
| Host ephemeral port range | 32768–60999 |
| Docker version | 29.8.1 |
| Test run date | 2026-10-18 |

### 2.2 Gateway Container Configuration

| Parameter | Value |
| --- | --- |
| Epoll workers | 8 (via `--epoll-workers 8`) |
| Pool threads | 16 (via `--pool-threads 16`) |
| Task queue capacity | 65,536 (via `--queue-capacity 65536`) |
| Listen backlog | 65,535 (via `--backlog 65535`) |
| Rate-limit capacity | 1,000,000 (via `--rate-limit-capacity 1000000`) |
| Rate-limit refill rate | 0 (disabled, via `--rate-limit-refill-rate 0`) |
| `GATEWAY_TEST_MODE` | Set (enables `GET /test/hold`) |
| Container `nofile` limit | 200,000 |

> [!NOTE]
> The rate-limit override prevents the shared generator IP from exhausting the token bucket. Production defaults (burst 10, refill 5/s) are preserved; only the test Compose service overrides them. This isolates load behaviour from rate-limit policy.

### 2.3 Load Generator Container Configuration

| Parameter | Value |
| --- | --- |
| Image | `python:3.12-alpine` |
| Container `nofile` limit | 200,000 |
| Source port range (`ip_local_port_range`) | 1024–65535 |
| Connect timeout per socket | 120 s |
| Response read deadline | 180 s |
| Stage release deadline | 300 s |

The generator opens all target connections before sending any requests (connection barrier). The controller waits for the gateway's `gateway_connections_accepted_total` counter to reflect the expected count before releasing. This separates the connection-establishment phase from the request-submission phase, allowing clean measurement of each.

### 2.4 Metric Definitions

| Metric | Definition |
| --- | --- |
| Connected / failures | Successful TCP connects / connect errors from generator |
| Responses | HTTP status codes observed in generator |
| Acknowledged writes | `201 Created` responses recorded by the generator |
| Recovered | Records found on restart that match acknowledged payloads |
| Connect time (s) | Wall time from first to last connect attempt |
| Request submit (s) | Wall time from first to last `sendall()` call |
| Offered req/s | `total_sent / request_submit_seconds` |
| Avg publish latency | Mean latency of `POST /publish` responses (excludes hold) |
| P95 publish latency | 95th-percentile of `POST /publish` latencies |
| Last publish response | Maximum per-client latency across all publish responses |
| All responses (s) | Client-measured elapsed time including the 15-second hold |
| SIGTERM drain (s) | Wall time from `docker compose stop` call to container exit |
| Pool queue peak | Peak `gateway_thread_pool_queue_high_water` observed |
| Peak completion queue | Maximum per-worker `gateway_epoll_worker_completion_queue_high_water` |
| Peak CPU | Highest `docker stats` CPU percentage sample |
| Peak RSS | Highest `docker stats` memory usage sample |

> [!NOTE]
> The shared task queue high-water is an aggregate metric across all epoll workers. Per-epoll-worker task-queue instrumentation does not exist in this design; only completion-queue depth/high-water are per-worker. Peak CPU and RSS are sampled at approximately 1-second intervals; they represent observed maxima, not guaranteed instantaneous peaks. The controller's `/metrics` scrapes are included in gateway counters, so accepted-connection and request totals are slightly above the load cohort counts.

---

## 3. Test Procedure

Each stage followed this protocol:

1. **Build** the gateway Docker image once before the stage sequence begins.
2. **Start gateway** with stage-specific storage directory, fixed configuration (8 workers, 16 pool threads, queue 65536, backlog 65535), test rate-limit override, and `GATEWAY_TEST_MODE`.
3. **Wait for readiness** by polling `/metrics` until reachable.
4. **Open connections**: generator establishes target N concurrent TCP connections; reports count to controller via `ready-<stage>` file.
5. **Wait for acceptance**: controller polls `gateway_connections_accepted_total` until it reflects the connected count (up to 10 s).
6. **Release connections**: controller writes `release-<stage>` file; generator begins sending requests.
7. **Submit requests**:
   - Client 0: `GET /test/hold` (15-second deliberate hold, exercises in-flight shutdown path).
   - Clients 1…N-1: `POST /publish` with unique payload `stage-<S>-record-<ID>`.
8. **Wait for `LOAD_IN_PROGRESS` marker** in generator logs.
9. **Take shutdown snapshot**: scrape `/metrics` to estimate in-flight count.
10. **Issue SIGTERM** via `docker compose stop --timeout 120`.
11. **Wait for clean exit**, polling `/metrics` and `docker stats` during drain.
12. **Collect generator results** after `docker wait` completes.
13. **Run storage recovery verifier**: `./build/storage_recovery_verify <storage_dir> <ack_file>` compares acknowledged payloads against recovered records.
14. **Restart gateway** against the same storage directory; confirm it becomes ready (re-reads recovered log).
15. **Stop restarted gateway** cleanly.
16. **Record all metrics** to `build/backend-test-results/stage-results.jsonl`.

---

## 4. Stage Results

### 4.1 Load and Latency Summary

| Concurrent clients | Connected / failures | Responses | Acknowledged writes / recovered | Connect time (s) | Request submit (s) | Offered req/s | Avg publish latency (ms) | P95 publish latency (ms) | Last publish response (s) | All responses (s) | SIGTERM drain (s) | Pool queue peak | Peak completion queue | Peak CPU | Peak RSS |
| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 100 / 0 | 1 × 200, 99 × 201 | 99 / 99 | 0.011 | 0.0012 | 82,520 | 2.71 | 3.23 | 0.0037 | 15.001 | 13.231 | 11 | 6 | 0.51% | 4.6 MiB |
| 500 | 500 / 0 | 1 × 200, 499 × 201 | 499 / 499 | 0.059 | 0.0169 | 29,644 | 19.84 | 22.77 | 0.0233 | 15.003 | 13.278 | 4 | 4 | 1.10% | 5.3 MiB |
| 1,000 | 1,000 / 0 | 1 × 200, 999 × 201 | 999 / 999 | 0.065 | 0.0228 | 43,894 | 31.68 | 34.62 | 0.0353 | 15.001 | 13.226 | 4 | 12 | 1.47% | 7.0 MiB |
| 2,500 | 2,500 / 0 | 1 × 200, 2,499 × 201 | 2,499 / 2,499 | 0.227 | 0.0398 | 62,879 | 65.38 | 68.99 | 0.0694 | 15.004 | 11.200 | 7 | 6 | 4.12% | 13.2 MiB |
| 5,000 | 5,000 / 0 | 1 × 200, 4,999 × 201 | 4,999 / 4,999 | 0.428 | 0.0796 | 62,789 | 133.58 | 140.51 | 0.1416 | 15.004 | 11.233 | 7 | 15 | 8.55% | 24.9 MiB |
| 10,000 | 10,000 / 0 | 1 × 200, 9,999 × 201 | 9,999 / 9,999 | 0.817 | 0.1285 | 77,814 | 229.27 | 248.12 | 0.2506 | 15.012 | 11.216 | 145 | 42 | 18.69% | 44.6 MiB |
| 20,000 | 20,000 / 0 | 1 × 200, 19,999 × 201 | 19,999 / 19,999 | 1.772 | 0.2218 | 90,154 | 437.06 | 477.75 | 0.4840 | 15.009 | 11.220 | 11 | 41 | 19.99% | 86.9 MiB |
| 30,000 | 30,000 / 0 | 1 × 200, 29,999 × 201 | 29,999 / 29,999 | 2.207 | 0.3441 | 87,177 | 700.27 | 753.07 | 0.7614 | 15.010 | 11.276 | 56 | 24 | 25.15% | 128.1 MiB |
| 50,000 | 50,000 / 0 | 1 × 200, 49,999 × 201 | 49,999 / 49,999 | 76.351 | 0.9366 | 53,386 | 1,604.7 | 1,704.2 | 1.7145 | 15.019 | 11.308 | 88 | 42 | 26.55% | 212.3 MiB |

### 4.2 Zero-Error Verification

At every stage: no connection failures, no send failures, no response-read failures, no `429 Too Many Requests`, no `503 Service Unavailable`, no HTTP parse errors.

The `inflight_estimate_at_shutdown` was **1** at every stage, corresponding exclusively to the deliberate `GET /test/hold` request. All `POST /publish` requests had been fully served and their `201 Created` responses received before SIGTERM was issued. This confirms that the bulk write workload completed well before shutdown under these test conditions, but it means graceful drain was not stress-tested with many simultaneous blocked application handlers — only one was in flight per stage.

The peak active connection gauge observed by the controller was the target count plus one (the controller's own metrics-scrape connection). Accepted-connection totals likewise include readiness and metrics probes.

---

## 5. Graceful Shutdown Under Load

### 5.1 Protocol

At each stage, SIGTERM was issued while exactly one handler (`GET /test/hold`, sleeping 15 s) remained in flight. The gateway was signalled using `docker compose stop --timeout 120`. The controller polled `/metrics` and `docker stats` throughout drain.

### 5.2 Shutdown Results

| Concurrent clients | In-flight at shutdown | Hold completed | Drain (s) | Exit code |
| ---: | ---: | --- | ---: | --- |
| 100 | 1 | Yes (200 OK, body "held") | 13.231 | 0 |
| 500 | 1 | Yes | 13.278 | 0 |
| 1,000 | 1 | Yes | 13.226 | 0 |
| 2,500 | 1 | Yes | 11.200 | 0 |
| 5,000 | 1 | Yes | 11.233 | 0 |
| 10,000 | 1 | Yes | 11.216 | 0 |
| 20,000 | 1 | Yes | 11.220 | 0 |
| 30,000 | 1 | Yes | 11.276 | 0 |
| 50,000 | 1 | Yes | 11.308 | 0 |

All nine stages performed a clean exit (exit code 0). The deliberately held handler returned `200 OK` with body `"held"` in every stage. Completion queues drained to zero depth at shutdown.

The drain duration of approximately 11.2–13.3 s is bounded by the 15-second hold handler less pool-startup latency. At stages 100–1,000 the total drain is slightly longer (~13 s); at stages 2,500–50,000 it converges to ~11.2 s. This reflects how quickly the gateway detects the `LOAD_IN_PROGRESS` marker and how much time remains in the hold's 15-second sleep when SIGTERM arrives. The held handler always completed before the 120-second compose-stop timeout.

### 5.3 Shutdown Behaviour Under These Conditions

After SIGTERM:

1. Workers stop accepting new connections and reject new pool submissions.
2. Idle or incomplete clients are closed immediately.
3. Accepted in-flight tasks continue to completion in pool workers.
4. Pool workers post completions through each worker's `eventfd`; the owning epoll thread flushes the response.
5. Each epoll worker exits only when its outstanding task count and active connection count both reach zero.
6. The server joins epoll threads and pool workers, then returns exit code 0.

> [!IMPORTANT]
> The test verifies this shutdown path at each concurrency level but with only one simultaneously blocked application handler. Scenarios with many handlers all blocked in application code simultaneously — for example, thousands of `GET /test/hold` requests — were not run. Peak publish latency is also not a shutdown-stress measurement; it reflects burst queue depth during normal serving.

---

## 6. Persistence Across Shutdown and Restart

### 6.1 Protocol

Each stage used a unique storage directory (`/tmp/backend-test-storage/<run_id>/stage-<N>`). Before issuing SIGTERM, clients 1…N-1 had each sent a unique payload `stage-<S>-record-<ID>` to `POST /publish`. The generator recorded every `201 Created` payload in `ack-<stage>.txt`.

After the gateway exited, `./build/storage_recovery_verify <storage_dir> <ack_file>` compared every acknowledged payload against the recovered segment files. The gateway was then restarted against the same storage directory to confirm it could reach ready state (which requires scanning and validating all segment files).

### 6.2 Persistence Results

| Concurrent clients | Acknowledged writes | Recovered | Recovery exit code | Verifier output |
| ---: | ---: | ---: | --- | --- |
| 100 | 99 | 99 | 0 | `acknowledged=99 records verified; recovered=99 total records` |
| 500 | 499 | 499 | 0 | `acknowledged=499 records verified; recovered=499 total records` |
| 1,000 | 999 | 999 | 0 | `acknowledged=999 records verified; recovered=999 total records` |
| 2,500 | 2,499 | 2,499 | 0 | `acknowledged=2499 records verified; recovered=2499 total records` |
| 5,000 | 4,999 | 4,999 | 0 | `acknowledged=4999 records verified; recovered=4999 total records` |
| 10,000 | 9,999 | 9,999 | 0 | `acknowledged=9999 records verified; recovered=9999 total records` |
| 20,000 | 19,999 | 19,999 | 0 | `acknowledged=19999 records verified; recovered=19999 total records` |
| 30,000 | 29,999 | 29,999 | 0 | `acknowledged=29999 records verified; recovered=29999 total records` |
| 50,000 | 49,999 | 49,999 | 0 | `acknowledged=49999 records verified; recovered=49999 total records` |

Every acknowledged write was recovered at every stage. Zero acknowledged records were missing; zero unacknowledged records were present. The restarted gateway reached ready state at all stages, confirming that segment scanning and recent-cache rebuild succeeded.

> [!NOTE]
> Each stage had one unacknowledged in-flight write at shutdown time: the `GET /test/hold` request for client 0. That request carried no `POST /publish` payload, so there was no unacknowledged persistent record to track. The test does not cover the scenario where many `POST /publish` requests are mid-flight at SIGTERM time, as the bulk writes completed before the signal was issued.

---

## 7. Per-Worker Measurements

Worker vectors are ordered worker IDs `0` through `7`. Accepted counts and request rates include controller metrics probes; they are approximate workload-distribution measurements, not exact client-only counts. Rates are computed over the request-submission interval only.

| Clients | Accepted connections by worker | Dispatched requests/s by worker | Completion queue high-water by worker |
| ---: | --- | --- | --- |
| 100 | 19, 7, 13, 8, 20, 11, 13, 14 | 15,679; 5,776; 9,902; 6,602; 16,504; 9,077; 9,902; 11,553 | 3, 1, 2, 1, 2, 2, 6, 2 |
| 500 | 78, 50, 52, 56, 67, 72, 65, 65 | 4,624; 2,964; 3,083; 3,261; 3,913; 4,269; 3,854; 3,854 | 4, 2, 1, 2, 2, 2, 3, 3 |
| 1,000 | 130, 131, 137, 106, 143, 135, 105, 118 | 5,706; 5,750; 6,013; 4,609; 6,277; 5,926; 4,565; 5,180 | 12, 9, 4, 2, 3, 6, 1, 4 |
| 2,500 | 328, 308, 312, 319, 293, 311, 319, 316 | 8,250; 7,747; 7,822; 8,023; 7,369; 7,822; 7,998; 7,948 | 6, 4, 5, 4, 4, 3, 6, 3 |
| 5,000 | 567, 637, 610, 669, 604, 627, 666, 626 | 7,120; 7,999; 7,648; 8,401; 7,585; 7,874; 8,351; 7,861 | 5, 7, 8, 15, 4, 6, 11, 7 |
| 10,000 | 1,244, 1,264, 1,211, 1,237, 1,286, 1,237, 1,268, 1,259 | 9,680; 9,828; 9,423; 9,626; 9,999; 9,626; 9,867; 9,797 | 42, 41, 31, 35, 40, 42, 40, 26 |
| 20,000 | 2,541, 2,495, 2,505, 2,540, 2,494, 2,483, 2,554, 2,394 | 11,454; 11,247; 11,292; 11,450; 11,242; 11,193; 11,508; 10,787 | 41, 25, 22, 27, 14, 24, 31, 23 |
| 30,000 | 3,751, 3,680, 3,831, 3,714, 3,727, 3,843, 3,706, 3,754 | 10,900; 10,695; 11,134; 10,794; 10,832; 11,170; 10,772; 10,912 | 14, 14, 24, 12, 13, 17, 16, 16 |
| 50,000 | 6,173, 6,207, 6,175, 6,221, 6,281, 6,275, 6,347, 6,327 | 6,591; 6,627; 6,593; 6,642; 6,706; 6,699; 6,776; 6,755 | 42, 25, 22, 28, 31, 32, 27, 35 |

`SO_REUSEPORT` distributes connections across all eight workers with reasonable balance. At 50,000 clients the eight workers each accepted about 6,200 connections (range: 6,173–6,347; coefficient of variation < 1%). The maximum observed per-worker completion-queue high-water mark was 42 (at 10,000 clients and again at 50,000). Per-worker completion totals closely followed dispatch totals, with occasional one-request differences at scrape time. Completion queues drained to zero depth at every stage during graceful shutdown.

The 30,000-stage per-worker rates are derived from the recorded per-worker request counters and the submission interval because the controller missed the final metrics scrape during that stage.

---

## 8. C++ Unit Test Results

Run with:
```bash
cmake -S . -B build
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Final result: **31/31 passed** (0.23 s total).

### 8.1 Test Cases by Target

**`http_parser_tests` (5 cases)**

| Case | Result |
| --- | --- |
| `parses_fragmented_headers_and_body` | PASS |
| `parses_request_without_body` | PASS |
| `separates_query_from_request_path` | PASS |
| `rejects_malformed_and_unsupported_requests` | PASS |
| `enforces_limits` | PASS |

**`rate_limiter_tests` (5 cases)**

| Case | Result |
| --- | --- |
| `token_bucket_allows_burst_and_refills` | PASS |
| `token_bucket_caps_refill_at_capacity` | PASS |
| `token_bucket_does_not_refill_without_elapsed_time` | PASS |
| `rate_limiter_tracks_ips_independently` | PASS |
| `router_rejects_before_handler_dispatch` | PASS |
| `rate_limiter_serializes_concurrent_consumption_per_ip` | PASS |

**`metrics_tests` (3 cases)**

| Case | Result |
| --- | --- |
| `metrics_track_connection_lifecycle` | PASS |
| `metrics_track_requests_responses_and_duration` | PASS |
| `metrics_track_parse_errors_and_uptime` | PASS |

**`config_tests` (2 cases)**

| Case | Result |
| --- | --- |
| `config_reads_file_and_cli_overrides` | PASS |
| `config_rejects_invalid_values_and_unknown_keys` | PASS |

**`thread_pool_tests` (2 cases)**

| Case | Result |
| --- | --- |
| `pool_bounds_queue_and_drains_accepted_tasks` | PASS |
| `pool_survives_task_exceptions` | PASS |

**`tcp_server_drain_tests` (1 case)**

| Case | Result |
| --- | --- |
| `shutdown_drains_an_in_flight_handler` | PASS |

This test appends three messages (`"first"`, `"second"`, `"quoted \"value\"\\line\n"`), verifies chronological ordering and JSON escaping through `/recent`, verifies `count=1` returns only the newest message, confirms `count=0` and `count=1001` boundaries, confirms `gateway_log_disk_bytes 256` in `/metrics`, starts a `/blocked` handler, signals SIGTERM, releases the handler, and confirms the response is delivered and `gateway_thread_pool_workers 1` is present in the post-shutdown metrics exposition.

**`mmap_log_tests` (8 cases)**

| Case | Result |
| --- | --- |
| `mmap_log_recovers_records_after_reopen` | PASS |
| `mmap_log_rolls_segments_and_tracks_global_offsets` | PASS |
| `mmap_log_truncates_a_torn_active_tail_and_resumes` | PASS |
| `mmap_log_discards_latest_record_with_bad_checksum` | PASS |
| `mmap_log_rejects_records_that_cannot_fit` | PASS |
| `mmap_log_truncates_payload_left_before_record_header` | PASS |
| `mmap_log_truncates_a_record_with_a_partial_payload` | PASS |
| `mmap_log_fails_on_corruption_in_a_sealed_segment` | PASS |

**`circular_buffer_tests` (4 cases)**

| Case | Result |
| --- | --- |
| `circular_buffer_preserves_order_before_wraparound` | PASS |
| `circular_buffer_returns_last_values_after_wraparound` | PASS |
| `circular_buffer_handles_exact_capacity_and_zero_count` | PASS |
| `circular_buffer_supports_capacity_one` | PASS |

---

## 9. HTTP Integration Test Results

Run with:
```bash
./scripts/run_http_integration_test.sh
```

This uses Docker Compose to start the compiled gateway in one container and a `python:3.12-alpine` test container beside it. The gateway is configured with `--rate-limit-capacity 1000 --rate-limit-refill-rate 0` so integration tests (all from the same container IP) do not exhaust a security token bucket.

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

The suite exercises the real compiled gateway over TCP using Python's standard-library `socket`. It verifies: normal request framing and response headers; byte-by-byte delivery with inter-byte delays; fragmented body via `Content-Length`; malformed request lines, headers, and HTTP versions; `Transfer-Encoding` rejection; header normalization; conflicting lengths; header and body size-limit rejection; exact routing and `404`; connection closure after one response; 100 concurrent raw-socket clients; a slow reader consuming the 512 KiB `/large-response` body in 1 KiB chunks; `/recent` cache response, count validation (zero, newest-only, malformed, unknown, repeated, over-limit); `/metrics` Prometheus text format and counter/gauge/histogram samples across repeated scrapes; and `/publish` empty-body rejection and opaque-body persistence visibility.

The gateway exit code during Compose teardown is `137` (SIGKILL from `--abort-on-container-exit`); that is expected and does not indicate a test failure.

---

## 10. Overall Validation

| Test category | Result |
| --- | --- |
| C++ CTest suite | **31/31 passed** |
| HTTP integration suite (Docker) | **13/13 passed** |
| Staged HTTP concurrency (100–50,000) | **9/9 stages passed** |
| Graceful shutdown under load | **9/9 clean exits** |
| Persistence recovery | **9/9 exact acknowledged-record sets recovered** |

---

## 11. Observations and Interpretation

### 11.1 Offered throughput vs. concurrency

The offered request rate is not a sustained-rate benchmark. It measures the time the generator takes to call `sendall()` on every socket after all connections are open. At 100 clients the window is 1.2 ms (82,520 req/s). At 50,000 clients the window is 937 ms (53,386 req/s), not because the gateway is slower, but because the Python generator itself requires more time to iterate 50,000 socket objects and call `sendall()` on each.

### 11.2 Latency scaling

Average `POST /publish` latency grows roughly linearly with client count up to ~20,000 clients, then flattens slightly. At 10,000 clients the pool task-queue high-water mark jumped to 145 (vs. single-digit values at lower stages), suggesting that the shared 16-thread pool temporarily queues work during the initial burst. At 20,000–50,000 clients the queue high-water drops back to 11–88, likely because the generator's submission window is long enough that requests arrive more spread out per pool worker.

### 11.3 Memory scaling

RSS scales approximately linearly: 212 MiB at 50,000 clients (~4.2 KiB per open connection socket + buffers). The per-connection outbound-buffer limit of 1 MiB bounds worst-case memory per slow client, but does not bound idle-connection overhead.

### 11.4 50,000-client connection time

The 76-second connection-establishment time at 50,000 clients dwarfs all other stages. This is dominated by the generator sequentially creating 50,000 sockets in Python. It is not a gateway bottleneck: the gateway was ready and accepting throughout that window.

### 11.5 Metric instrumentation limits

Per-epoll-worker task-queue depth and high-water do not exist in this design. The shared pool queue length and high-water are aggregate metrics. The per-worker completion-queue high-water and depth are the only per-worker queue metrics available. Sustained-rate benchmarking and per-worker task-queue instrumentation remain future work.

### 11.6 Durability caveat

The test uses graceful process shutdown and reopen recovery. It does not simulate sudden power loss or kernel crash. The mmap-backed storage provides ordered, CRC-checked records visible immediately after write, but durability against power failure requires the `flush()` / `msync(MS_SYNC)` / `fsync()` path, which is called on segment rollover and normal close but not after every individual append.

---

## 12. Raw Data

Stage-level JSONL records (initial metrics, ready metrics, max metrics, final worker metrics, load summary, graceful shutdown, recovery, docker stats samples, restart metrics) are stored at:

```
build/backend-test-results/stage-results.jsonl
```

The acknowledged-write files are at `build/backend-test-results/ack-<stage>.txt`. All files are excluded from version control (`.gitignore`).
