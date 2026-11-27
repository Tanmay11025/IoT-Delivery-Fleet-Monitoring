# Results and Conclusions

## 1. Executive Summary

Staged burst run (2026-10-18): all test categories passed. Detailed environment, metric definitions, per-worker vectors, stage data, and analytical notes are in [tests.md](tests.md).

| Test category | Outcome |
| --- | --- |
| C++ CTest (31 cases) | **31/31 passed** |
| Docker HTTP integration (13 cases) | **13/13 passed** |
| Staged HTTP concurrency — 9 stages | **9/9 passed** |
| Graceful shutdown under load | **9/9 clean exits** |
| Persistence recovery across restart | **9/9 exact acknowledged-record sets recovered** |

Later runs (2026-11-24, after the fleet features; Release build):

| Test category | Outcome |
| --- | --- |
| C++ CTest (51 cases) | **51/51 passed** (also under ASan + UBSan) |
| pytest integration + chaos (38 cases) | **38/38 passed** (the earlier 34-case suite also passed 5 repeat runs) |
| Docker HTTP integration (13 cases) | **13/13 passed** |
| `kill -9` during publishing | **every acknowledged record recovered** |
| Sustained throughput (54 `wrk` runs) | **~77,000 events/s at 1.03 ms P99** (120 B); **0 errors** |

> **Note on the 2026-10-18 run.** The staged runs used the Docker image built with a plain
> `cmake -S . -B build`. CMake adds no optimisation flags when no build type
> is set, so this binary was unoptimised and its latencies are pessimistic.
> The correctness, capacity and recovery conclusions are unaffected. CMake
> now defaults to Release (see [journal.md](journal.md)). Sustained
> throughput, which these burst runs do not measure, is reported in §6.
> Telemetry validation and consumer delivery, missing at the time of this
> run, are now implemented and tested; see §7 and
> [architecture.md](architecture.md).

---

## 2. Build and Unit-Test Results

### 2.1 Full suite

The full C++ suite has 51 CTest cases across 11 test executables. Latest
run (2026-11-24, Release build):

```bash
cmake -S . -B build            # defaults to Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

```text
100% tests passed, 0 tests failed out of 51
Total Test time (real) = 0.27 sec
```

The same 51 cases also pass in a Debug build with AddressSanitizer and
UndefinedBehaviorSanitizer (the CI `sanitizers` job). The 2026-10-18 staged
run in §4 used the 31 cases that existed at the time.

### 2.2 Covered behaviors

| Test target | Cases | Behaviors verified |
| --- | ---: | --- |
| `http_parser_tests` | 5 | Fragmented headers and bodies; `Content-Length` completion; requests without bodies; case-insensitive header lookup; query/path separation; malformed requests; conflicting lengths; unsupported chunked encoding; header/body size limits |
| `rate_limiter_tests` | 6 | Fake-clock token-bucket burst/refill/cap; zero-rate behavior; independent IP buckets; `429` handler bypass; concurrent same-IP consumption capped at capacity |
| `metrics_tests` | 4 | Connection lifecycle; request/blocked counts; response classes; duration histogram; parse errors; process uptime; log disk-byte gauge; telemetry accepted/rejected counters; vehicle gauges; consumer count, records sent and lag |
| `config_tests` | 3 | Config-file parsing; CLI override precedence; invalid-value rejection; unknown-key rejection; `consumer_port` (including 0 = disabled and clash with `port`), `max_consumers`, `dashboard_directory`, `vehicle_offline_seconds` |
| `thread_pool_tests` | 2 | Fixed worker stats; bounded-queue rejection; rejection after shutdown starts; draining of accepted tasks; continued worker operation after a task throws |
| `tcp_server_drain_tests` | 1 | Three messages appended through API; chronological ordering in `/recent`; `count=1` selects only the newest; JSON escaping; `count=0` and `count=1001` boundary validation; `gateway_log_disk_bytes` in `/metrics`; in-flight handler released after SIGTERM; response delivered before server thread joins; `gateway_thread_pool_workers` present in post-shutdown exposition |
| `mmap_log_tests` | 10 | Append/read across close and reopen; variable and empty payloads; global offsets across segment rollover; exact-fit rollover; oversized-record rejection; incomplete-header/payload recovery; payload bytes before a missing header; active-tail checksum rejection; fatal corruption in a sealed segment; `read_from` across segments, mid-segment starts, byte budgets, end/past-end/non-boundary offsets and after reopen; on-disk CRCs equal a reference CRC-32/IEEE (`"123456789"` → `0xCBF43926`) |
| `circular_buffer_tests` | 4 | Chronological reads before and after wraparound; exact-capacity fills; zero/oversized reads; capacity-one replacement |
| `telemetry_event_tests` | 6 | Valid event; field order, whitespace and ignored unknown scalar fields; `\uXXXX` escapes; missing and mistyped fields; range checks (lat, lng, speed, status, vehicle_id charset/length, timestamp); malformed JSON (trailing data, duplicate keys, nesting, bad numbers, raw control characters, lone surrogates, oversize body) |
| `fleet_state_tests` | 6 | Latest report per vehicle; out-of-order reports ignored by offset; offline after the silence window (fake clock); age restored when rebuilt from the log; JSON output and counts; 8 threads updating concurrently keep each vehicle's highest offset |
| `frame_codec_tests` | 4 | Big-endian encoding of FETCH_FROM and DATA; reassembly from one-byte reads; several frames in one read; zero length, oversized and unknown-type frames rejected |

---

## 3. HTTP Integration Test Results

Run with `./scripts/run_http_integration_test.sh`. Docker Compose starts the compiled gateway and a Python `python:3.12-alpine` test container. The gateway's rate-limit override (`--rate-limit-capacity 1000 --rate-limit-refill-rate 0`) prevents the shared container IP from exhausting the token bucket.

**Result: 13/13 passed**, both on 2026-10-18 and again on 2026-11-24 after
telemetry validation was added (the publish case now sends a valid
TelemetryEvent). Output of the 2026-11-24 run:

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
PASS test_publish_appends_and_recent_recovers_message
PASS all HTTP integration tests (13)
http-test exited with code 0
```

In the 2026-10-18 run the gateway container exited with `137` during Compose
teardown (`SIGKILL` from `--abort-on-container-exit`), which is not a test
failure. In the 2026-11-24 runs it exited with `0`.

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
- `/publish` rejects an empty body with `400`; a valid TelemetryEvent returns `201 Created` with `{"offset":N}` and is immediately visible through `/recent`.

### 3.2 Troubleshooting note — rate limiter vs. integration tests

The first HTTP integration run after adding per-IP rate limiting failed: all test clients shared the same Docker container IP, so earlier tests consumed the default 10-token bucket. Subsequent tests received `429 Too Many Requests`. Binding clients to different loopback addresses was not portable inside the Alpine network namespace. The final fix was to configure the integration Compose service with a large capacity and zero refill rate. This preserves the production default while making transport-layer tests independent of the rate-limiting policy.

### 3.3 pytest integration and chaos suites (2026-11-24)

Run with `python3 -m pytest`. Each test starts a real `message_gateway`
process on free local ports with its own storage directory
(`tests/conftest.py`), so they test the binary end to end, including
restarts and crashes.

**Result: 35/35 passed** (about 9 s). The earlier 34-case suite also passed 5
consecutive repeat runs, and the 33 integration cases pass against the
ASan/UBSan build.

| File | Cases | What it checks |
| --- | ---: | --- |
| `integration/test_telemetry_publish.py` | 10 | Valid event stored and shown in `/vehicles`, `/recent`, metrics; 6 invalid bodies get `400` with the exact reason and change nothing; SDK raises `ValidationError`; latest event wins per vehicle; fleet view identical after restart |
| `integration/test_sdk_roundtrip.py` | 9 | Replay returns every record in order with chained `next_offset`; replay from the middle; ~2 MB replay (many times the 256 KiB queue) completes; live subscriber gets exactly the new records; ACKs reach `/consumers` with zero lag; request frames split across sends and responses read 7 bytes at a time; non-boundary and past-end offsets get `ERROR`; a consumer sending `DATA` gets `ERROR` and is closed |
| `integration/test_consumer_reconnect.py` | 2 | Consumer "crashes" after 40 of 100 records and a new process resumes from the offset file with no gap and no duplicate; gateway `kill -9` + restart while a consumer streams, and all 100 records are still delivered |
| `integration/test_dashboard_static.py` | 12 | `/` and the 5 whitelisted assets served with correct content types; 6 traversal or unknown paths return `404` |
| `chaos/test_kill9.py` | 1 | 4 publishers × 250 events; `kill -9` after 600 acknowledgements; after restart every acknowledged record is present byte-for-byte, each vehicle's position is at least as new as its last acknowledged report, and appends continue at the right offset |
| `chaos/test_graceful_shutdown.py` | 1 | SIGTERM while 4 publishers send and a consumer streams; exit code 0; consumer sees a clean end of stream; every acknowledged record recovered |

The large-replay case was checked against the bug it guards: with the old
pump behaviour it failed ("replay stalled after 260 of 2000 records").

---

## 4. Staged HTTP Concurrency Results

Run with `python3 scripts/run_backend_tests.py`. The runner sequenced all nine stages. Each stage opened N connections, sent `POST /publish` to N-1 clients and `GET /test/hold` to one, then triggered shutdown and verified recovery.

These results are from the 2026-10-18 run, when `/publish` stored opaque bodies. The harness (`tests/backend_load_stage.py`) has since been changed to send valid TelemetryEvent JSON so it still passes validation. **The staged test has not been re-run since that change**, so the table below still reflects the October build.

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

## 6. Sustained Throughput Benchmark

The staged tests above open many connections and send one request each, so
they measure burst capacity, not sustained throughput. This section measures
the full publish path under continuous load: parse → rate-limit check →
telemetry validation → log append → fleet-state update → response. It was
run on 2026-11-24, after telemetry validation and the consumer protocol were
added, using a Release build.

**Headline:** on one laptop, the gateway sustains **~77,000 validated
telemetry events/s at 1.03 ms P99** (120-byte events, 100 connections), and
**~35,000 events/s with 8 KiB bodies (~280 MB/s into the log)**. It had zero
errors in 54 benchmark runs. This is simulated load from `wrk` on the same
machine, not real vehicles.

### 6.1 Setup

| Item | Value |
| --- | --- |
| CPU | Intel Core i5-1240P (4 P-cores + 8 E-cores, 16 threads), laptop, frequency scaling on |
| OS | Ubuntu, Linux 7.0 |
| Build | CMake `Release` (`-O3`), GCC |
| Gateway | 4 epoll workers, pool threads varied, queue 4096, rate limit effectively off, 256 MiB segments, fresh empty log per run |
| Placement | gateway pinned to CPUs 0–7 (P-cores), `wrk` to CPUs 8–15 (E-cores) with `taskset` |
| Load | `wrk -t4`, 2 s warm-up, then 8 s measured, **3 repeats per cell, median reported** |
| Request | valid TelemetryEvent JSON (`tests/benchmarks/telemetry.lua`); 1 KiB and 8 KiB bodies padded with an ignored `note` field |
| Connections | one request per connection (the gateway sends `Connection: close`), so every request includes a TCP handshake |

Reproduce with `tests/benchmarks/run_bench.sh`. Raw data:
[`tests/benchmarks/results/matrix-2026-11-24.csv`](../tests/benchmarks/results/matrix-2026-11-24.csv).

### 6.2 Results

| Pool threads | Body | Connections | Req/s (median) | P50 ms | P90 ms | P99 ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 120 B | 100 | 77,040 | 0.28 | 0.70 | 1.03 |
| 2 | 120 B | 1000 | 69,985 | 2.66 | 4.84 | 8.65 |
| 2 | 1 KiB | 100 | 66,555 | 0.33 | 0.91 | 98.62 |
| 2 | 1 KiB | 1000 | 62,551 | 2.89 | 5.34 | 110.04 |
| 2 | 8 KiB | 100 | 35,136 | 2.44 | 3.46 | 98.16 |
| 2 | 8 KiB | 1000 | 34,712 | 25.34 | 40.38 | 131.57 |
| 4 | 120 B | 100 | 70,107 | 0.32 | 0.78 | 1.16 |
| 4 | 120 B | 1000 | 65,299 | 2.99 | 5.36 | 8.95 |
| 4 | 1 KiB | 100 | 64,705 | 0.34 | 0.94 | 101.13 |
| 4 | 1 KiB | 1000 | 60,700 | 2.96 | 5.30 | 123.75 |
| 4 | 8 KiB | 100 | 34,602 | 2.45 | 3.96 | 99.49 |
| 4 | 8 KiB | 1000 | 34,732 | 25.36 | 39.68 | 137.88 |
| 8 | 120 B | 100 | 68,903 | 0.31 | 0.78 | 1.28 |
| 8 | 120 B | 1000 | 63,983 | 2.96 | 5.43 | 8.99 |
| 8 | 1 KiB | 100 | 64,240 | 0.35 | 0.98 | 125.12 |
| 8 | 1 KiB | 1000 | 59,765 | 3.20 | 6.20 | 115.71 |
| 8 | 8 KiB | 100 | 33,364 | 2.49 | 15.90 | 118.48 |
| 8 | 8 KiB | 1000 | 31,521 | 26.28 | 96.23 | 627.31 |

All 54 runs: 0 non-2xx responses, 0 socket errors.

### 6.3 Interpretation

**More pool threads did not help, and slightly hurt.** Throughput is flat or
falling from 2 to 8 pool threads in every row. Each publish holds the single
log mutex for the append, and the work outside it (validation, fleet update)
is short. Extra threads therefore mostly queue on that mutex and compete with
the 4 epoll workers for the 8 P-core threads. At 8 threads with 8 KiB bodies
and 1000 connections, P90 rises from about 40 ms to 96 ms. That cell also had
the widest spread between repeats (P99 627 ms median), which fits CPU
oversubscription rather than a steady cost. The best setting on this machine
is 2 pool threads.

**Connections affect latency, not throughput.** Going from 100 to 1000
connections raises P50 about tenfold (0.28 → 2.66 ms) while requests/s drop
only about 9%. The extra clients just queue. This is Little's law: at a fixed
completion rate, ten times the outstanding requests means about ten times
the time each one waits.

**Body size: throughput in bytes keeps rising.** 120 B → 8 KiB is 68× more
data per request for about 2.2× fewer requests, so about 9 MB/s becomes
about 285 MB/s. Per-request overhead (TCP handshake, HTTP parse,
scheduling) dominates small events; per-byte work (CRC, JSON scan,
`memcpy`) dominates large ones.

**The P99 cliff at 1 KiB and above is segment rollover.** With 120-byte
events, P99 tracks P90 (about 1 ms). From 1 KiB up, P99 jumps to about 100 ms
while P50/P90 stay low. The cause: when a 256 MiB segment fills,
`append()` runs a synchronous `msync` + `fsync` of the whole segment
**while holding the log mutex**, so every publish waits behind it. At
65–280 MB/s a segment fills every 1–4 s, inside the 8 s window. A 120-byte
run writes only about 70 MB and never rolls over. The hypothesis was tested
directly with the same 1 KiB / 4 threads / 100 connections cell (2 repeats
each):

| Segment size | Rollovers in the run | Req/s | P50 ms | P99 ms |
| ---: | --- | ---: | ---: | ---: |
| 64 MiB | ~10 | 69,477 | 0.33 | 28.0 |
| 256 MiB (matrix default) | 2–3 | 64,705 | 0.34 | 101.1 |
| 1 GiB | 0 | 70,112 | 0.33 | 1.19 |

With no rollover the tail disappears. Smaller segments mean shorter but more
frequent stalls. The fix, not implemented, is to hand the sealed segment to
a background thread for `msync`/`fsync`/`munmap`, so appends to the new
segment never wait for it. This is listed as future work. Raw data:
`tests/benchmarks/results/rollover-segment-*.csv`.

### 6.4 The optimisation that was made

The first runs showed 8 KiB bodies at only about 10.5k req/s. `perf` needs
root on this machine (`perf_event_paranoid = 4`), so the gateway was profiled
with `valgrind --tool=callgrind` under an 8 KiB load:

| Function | Instructions |
| --- | ---: |
| `MemoryMappedLog::append` (bit-at-a-time CRC32, inside the log mutex) | 61.2% |
| `FlatJsonReader::read_string` (one `+=` per character) | 30.9% |

After a compile-time CRC table (bit-identical output, enforced by a golden
test) and run-based string copying, measured with 4 pool threads and 100
connections:

| Body | Before | After |
| ---: | ---: | ---: |
| 120 B | 69.5k req/s | 74.6k req/s |
| 8 KiB | 10.5k req/s | 33.1k req/s (**3.2×**) |

The full story is in [journal.md](journal.md). To profile with `perf`
instead (needs root):

```bash
sudo perf record -g -p "$(pgrep -n message_gateway)" -- sleep 10   # while wrk runs
sudo perf report --stdio --no-children | head -40
```

### 6.5 Cache vs. log reads

`./build/cache_vs_disk_bench` (Release) times returning the newest 100
records from a 200,000-record log:

| Path | Time per call |
| --- | ---: |
| `get_recent(100)`: in-memory ring buffer (`/recent`) | 2.1 µs |
| `read_from(offset)`: one batched `pread` + CRC per record (consumer replay) | 68.7 µs |
| 100 × `read_from(…, 1 record)` from a sealed segment | 154.7 µs |

The cache is about 33× faster than a batched log read, even though the log
files are hot in the page cache. These numbers measure syscalls, copies and
checksums, not physical disk latency. This is why `/recent` and the fleet
rebuild use the ring buffer while consumers use the log, which is the source
of truth.

### 6.6 What these numbers do not show

- **Same-host loopback.** Client and server share one machine and memory
  bus, with no real network latency. Absolute numbers will differ on servers
  and across a network.
- **Laptop CPU.** Frequency scaling and the P/E-core split add noise: repeats
  of one cell differ by a median of 2% in throughput (worst cell 9%) and much more in P99.
- **No keep-alive.** Every request pays for a TCP handshake. A keep-alive
  client would show higher throughput, which reflects the current gateway
  honestly but differs from typical HTTP benchmarks.
- **Rate limiting disabled** for the benchmark. With the default policy a
  single IP is limited to 5 requests/s by design.
- **Durability level.** Throughput is measured with page-cache durability
  (survives `kill -9`), not per-record `fsync`.

---

## 7. Conclusions

### 7.1 What the results demonstrate

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
| Telemetry validation | `telemetry_event_tests` (6 cases) + `test_telemetry_publish.py` |
| Acknowledged writes survive a process crash | `tests/chaos/test_kill9.py` (kill -9 while 4 publishers send) |
| Consumers resume without gaps | `test_consumer_reconnect.py` (consumer crash and gateway kill -9) |
| Sustained throughput and tail latency | §6: 54 `wrk` runs, zero errors |
| On-disk CRC format unchanged by optimisation | `mmap_log_writes_standard_crc32_checksums` |

### 7.2 What the results do not demonstrate

- **Sustained throughput on server hardware**: §6 measures sustained throughput, but only on one laptop over loopback with no keep-alive.
- **Many simultaneous blocked handlers**: only one handler was in flight at SIGTERM. Shutdown drain under thousands of blocked application handlers was not tested.
- **Power-loss durability**: `kill -9` recovery is verified, but a power loss or kernel crash is not; records are not `fsync`'d one by one.
- **Multi-host or production network**: all measurements are on a single-host Docker bridge network.
- **Rollover tail latency**: with bodies of 1 KiB or more, P99 is about 100 ms because a full segment is synced while appends wait (§6.3). This is measured but not fixed.
- **Authentication and sustained rate-limit effectiveness**: token-bucket correctness is verified by unit tests; a sustained security-load benchmark under hostile traffic was not run.
- **Inactive-IP bucket expiration**: the limiter map grows without bound; high-cardinality IP traffic was not tested.

### 7.3 Overall assessment

The gateway has a proven event-driven transport foundation: it accepts, serves, and cleans up tens of thousands of simultaneous HTTP clients, shuts down gracefully while in-flight work completes, and recovers all acknowledged writes from persistent storage across process restart. Telemetry validation, the fleet view, and consumer streaming with at-least-once delivery have since been added and tested on top of it. They sustain about 77,000 validated events/s on a laptop. The main remaining gaps are authentication/TLS, the rollover stall, and multi-node operation.
