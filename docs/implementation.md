# Implementation Guide

## 1. Project Purpose

This project is a Linux-based gateway for delivery-fleet telemetry. Vehicles
and connected devices send location, speed, status, and timing data over
HTTP. The gateway accepts many connections, validates each report, stores it
in a crash-safe append-only log, keeps a live view of every vehicle for the
dashboard, and streams stored reports to downstream consumers over a binary
protocol.

The project was built in layers. We started with the smallest useful network
program, proved that the operating-system primitives worked, and then added
scalability, safety, HTTP handling, and telemetry-oriented contracts. This
order was chosen because each layer depends on the previous one. It is easier
to debug a reliable TCP foundation before adding parsing and application logic.

## 2. Implementation History

### 2.1 Build and runtime configuration

The gateway is a CMake-based C++17 executable built with `-Wall`, `-Wextra`,
and `-Wpedantic`. When no `CMAKE_BUILD_TYPE` is given, CMake now defaults to
`Release`; before this change such builds (including the Docker image) had no
optimisation flags at all. `Config` centralizes the listening port, backlog,
epoll worker count, request-pool size and queue capacity, log path, and
rate-limit policy. It also centralizes the log directory, segment size,
recent-cache capacity, the consumer port (`consumer_port`, 0 disables it) and
connection cap (`max_consumers`), the dashboard directory
(`dashboard_directory`), and the offline window (`vehicle_offline_seconds`).
Values load from a simple `key=value` file, and CLI options override file
values. Configuration is validated before server startup; `config.example.ini`
shows the supported settings.

### 2.2 Socket lifecycle and nonblocking I/O

The server follows `socket -> setsockopt -> bind -> listen -> accept ->
read/send -> close`. Listening and client sockets are nonblocking. `EAGAIN` and
`EWOULDBLOCK` mean the current I/O operation has drained for now; `EINTR` is
retried. Socket cleanup is centralized in `TCPServer` helpers.

### 2.3 Logging and event loop

`Logger` serializes timestamped output and can write to the configured log
path. `EpollLoop` uses level-triggered Linux `epoll`; it drains readable data
until `EAGAIN`, enables `EPOLLOUT` only when output is pending, and runs a
periodic drain callback so shutdown progresses even without socket activity.

### 2.4 Connection ownership and buffering

Each epoll worker owns one HTTP listener, one consumer listener (when
`consumer_port` is non-zero), an event loop, and a `ConnectionManager`.
Connections contain their kind (HTTP or consumer), HTTP parser,
inbound/outbound buffers, partial-write offset, peer-close state, in-flight
flag, and generation token. Consumer connections also hold a frame decoder,
a cursor (next log offset to send), the offset the consumer has acknowledged,
and a consumer ID. `ConnectionManager` keeps a separate set of consumer
descriptors, so new records can be pushed without scanning every HTTP
connection. A 1 MiB
per-connection output limit bounds slow-reader memory use. The generation token
prevents a late asynchronous response from being applied to a new connection
that reused the same file descriptor.

### 2.5 Epoll workers and request pool

`SO_REUSEPORT` distributes newly accepted connections among the configured
epoll workers. Because `SO_REUSEPORT` would equally let a *second* gateway
process bind the same port and silently take part of the traffic, `start()`
first probes the HTTP and consumer ports with a socket bound without it; if
either is already in use, the gateway exits with an error before opening its
log. Those workers keep all connection mutation and socket I/O
thread-confined. Once an HTTP request is complete, the epoll worker submits a
copy of the request and client IP to one shared, fixed-size `ThreadPool`. Its
bounded queue rejects excess work with `503 Service Unavailable`. Pool threads
perform routing and serialization, then post the result to the owning epoll
worker through a protected completion queue and `eventfd`. Only the owner
checks the connection generation, appends output, and sends the response.

### 2.6 Shutdown and failure handling

`SIGINT` and `SIGTERM` set an atomic shutdown flag. Epoll workers stop
accepting connections and new tasks, close idle or incomplete clients, and
remain active to process completions. The pool rejects submissions after
shutdown begins and drains all accepted tasks. Consumer connections are
closed immediately when the drain starts: they have no in-flight work, and a
consumer's position is its own saved offset, so it reconnects and resumes.
Each epoll loop exits only when
its outstanding tasks and connections are gone; the server then joins the
epoll threads and pool workers. Hard listener errors trigger the same drain
path, while client errors remove the descriptor and close the connection.

### 2.7 Persistent segmented log and recent cache

`MemoryMappedLog` stores append-only segment files in the configured
`storage_directory`. The default segment capacity is 64 MiB. Files are named
`log-00000001.seg`, `log-00000002.seg`, and so on, and are pre-sized with
`ftruncate()` before being mapped `MAP_SHARED` with read/write protection.
Records never cross segment boundaries; a new segment is created before an
append that will not fit.

The on-disk representation is explicitly serialized little-endian; it does not
write native C++ structs:

```text
40-byte segment header:
  magic[8] | version:u32 | segment_id:u32 | base_offset:u64
  segment_capacity:u64 | header_crc32:u32 | reserved:u32

32-byte record header, followed by payload bytes:
  record_magic:u32 | header_size:u32 | message_offset:u64
  timestamp_ns:u64 | payload_length:u32 | crc32:u32 | payload[payload_length]
```

The CRC covers the first 28 record-header bytes and the payload. Message
offsets start at zero and advance by each serialized record's header plus
payload size; segment headers are not part of the logical offset space. Each
segment index entry records its ID, path, first/end message offsets, and valid
physical byte boundary for later offset seeking. Payload bytes are copied
before the record header. The segment-header CRC covers its first 32 bytes;
the record CRC is CRC-32/IEEE over the first 28 record-header bytes followed by
the payload. Both CRC values are stored little-endian. The CRC was first
computed one bit at a time; profiling showed that loop as ~60% of CPU under
8 KiB publishes (it runs inside the log mutex), so it now uses a 256-entry
table built at compile time. The output is bit-identical, which
`mmap_log_writes_standard_crc32_checksums` enforces against a reference
implementation, so existing segment files remain valid.

Startup scans segments in ID order, validates segment headers and each record's
offset, bounds, and checksum, rebuilds the recent-message cache, and resumes at
the last valid record. A zero record magic is considered unused padding only
when every remaining byte in the segment is zero. This detects payload bytes
left behind if a crash occurs after payload copy but before header copy. Any
invalid or incomplete tail in the newest segment is truncated to the preceding
valid boundary; corruption in a sealed older segment fails startup rather than
silently discarding history. `flush()` and normal close/rollover call
`msync(MS_SYNC)` and `fsync()`, and report persistence errors. Appends are
serialized with the log mutex. An append is visible through the mapping and
recent cache immediately, but is not guaranteed durable until `flush()` or
normal close/rollover completes successfully; an unflushed append may be lost
on power failure. Explicit `flush()` and `close()` propagate failures to the
caller. The destructor also attempts a synchronized close, but reports a
failure to standard error because it cannot throw during destruction.

The configurable circular buffer retains the latest messages in RAM and is
rebuilt from the log on restart. Its `T` type must be default-constructible
(backing vector slots) and copyable (`get_recent()` returns copied values).
`CircularBuffer` itself is not thread-safe; `MemoryMappedLog` synchronizes
append and cache reads with its mutex. `GET /recent` serializes that cache
without scanning disk per request. It accepts an optional `count` query
parameter, defaults to 100, permits values from 0 through 1000, and returns
`400 Bad Request` for malformed, repeated, unknown, or over-limit query values.
The returned set is chronological, oldest-to-newest. The executable's
`POST /publish` route (since the fleet work) validates the body as a
TelemetryEvent, returning `400` with a reason when invalid, and otherwise
appends it and returns `201 Created` with `{"offset":N}`. The `gateway_log_disk_bytes` metric sums segment
file lengths and represents provisioned segment capacity, not only valid
record bytes.

### 2.8 Telemetry validation and fleet state

`parse_telemetry` (`src/telemetry/telemetry_event.cpp`) is a small
hand-written reader for exactly one flat JSON object. It supports strings
(with escapes, including `\uXXXX` outside the surrogate range), numbers,
`true`/`false`/`null`, and rejects nesting, duplicate keys, raw control
characters, trailing data, and bodies over 16 KiB. It then checks the six
required fields and their ranges (see [protocol.md](protocol.md)). Unknown
scalar fields are ignored so devices can send extras. A hand-written parser
was chosen over a JSON library because the format is fixed and flat, the
project builds everything from scratch, and each error can name the exact
field. Runs of plain characters are copied with one `append`; copying one
character at a time had shown up as ~30% of CPU for large bodies.

The publish handler (`TCPServer::publish_telemetry`) validates first, so the
log never holds records the system cannot interpret, then appends the
original bytes, updates `FleetState`, increments the accepted counter, and
returns `{"offset":N}`. A rejected body increments
`gateway_telemetry_rejected_total` and returns `400 {"error":"..."}`.

`FleetState` (`src/telemetry/fleet_state.cpp`) maps `vehicle_id` to the
latest event, its log offset, and when it arrived, behind a `shared_mutex`
(reads from `/vehicles` and `/metrics` run in parallel; updates are
exclusive). An update with a lower offset than the stored one is ignored, so
two pool threads finishing out of order cannot move a vehicle backwards. A
vehicle silent for longer than `vehicle_offline_seconds` is reported as
`offline` while its own last status is kept as `reported_status`. The clock
is injectable for deterministic tests. On startup the view is rebuilt from
the recovered recent cache; each record's age is derived from its stored
append timestamp so silent vehicles are still shown offline after a restart.
It is a derived view: the log remains the source of truth.

### 2.9 Consumer protocol, replay, and live streaming

Consumers connect to `consumer_port` and speak the length-prefixed frames in
[protocol.md](protocol.md) (`SUBSCRIBE`, `FETCH_FROM`, `DATA`, `ACK`,
`ERROR`). `FrameDecoder` (`src/protocol/frame_codec.cpp`) buffers bytes until
a complete frame is available, caps client frames at 1 KiB, and compacts its
buffer as frames are consumed. Consumer traffic is handled on the epoll
worker itself, not the pool: the work per frame is small, and it keeps all
socket state thread-confined like the HTTP path.

`MemoryMappedLog::read_from(offset, max_bytes)` serves replay. Offsets are
global byte positions, so it binary-searches the segment index by first
offset and computes the file position as `40 + (offset - first_offset)`. It
copies the index under the log mutex, then reads with `pread` outside it,
which is safe because bytes below the log end never change. Each record is
checked for magic, matching offset, bounds, and CRC; an offset that is not a
record boundary raises `invalid_argument`, which the server turns into an
`ERROR` frame.

`TCPServer::pump_consumer` fills a consumer's output queue from the log in
64 KiB batches until about 256 KiB is queued or it has caught up, then
flushes. If the kernel accepts everything, it loops and fills again; if the
socket would block, `EPOLLOUT` resumes it later. (An earlier version returned
after its first flush and could leave a consumer waiting forever during a
large replay; see [journal.md](journal.md).) The already-sent prefix of the
output buffer is reclaimed so long-lived streams do not grow memory.

New records reach live consumers without new cross-thread machinery. Each
worker registers its `LoopState`; after an append, the publishing pool thread
sets that worker's `data_available` flag and writes its existing completion
`eventfd`, only if the worker has consumers and only if the flag was not
already set. The worker clears the flag before reading the log end, so a
record appended while it pumps triggers another wake-up instead of being
missed. A small registry (behind its own mutex) records each consumer's
cursor and acknowledged offset for `GET /consumers` and the
`gateway_consumer_lag_bytes` metric. A global counter enforces
`max_consumers`; excess connections receive `ERROR "too many consumers"`.

### 2.10 Dashboard serving

`load_dashboard_files` (`src/http/static_files.cpp`) reads a fixed list of
six files (`index.html`, `style.css`, `app.js`, `charts.js`,
`fleet_map.js`, `city_map.json`) from `dashboard_directory` at startup, and each is
registered as an exact route under `/dashboard/`, with `index.html` also at
`/`. Because only exact routes exist, a path such as
`/dashboard/../config.ini` can never reach the filesystem. Missing files are
logged as a warning. Files are held in memory, so dashboard edits need a
restart. The dashboard itself is plain HTML/CSS/JavaScript (Chart.js from a
CDN is the only dependency); it polls `/vehicles` every 500 ms and `/metrics`, `/recent` and
`/consumers` every second, and computes rates using the gateway's
`process_uptime_seconds` as the time base. The fleet map draws the streets of
a fictional 6 x 3.4 km city (`city_map.json`, about 420 junctions and 177 km
of street, produced by `scripts/generate_city_map.py`), which
`examples/fleet_simulator.py` also routes its vans on: each van works
delivery rounds of 6-14 nearby addresses from one of four depots. The map can
be zoomed (scroll) and panned (drag); street names appear once they fit. Each van report is placed on a timeline at the moment the
gateway received it (poll time minus `age_seconds`), and the map is drawn
1.8 s behind real time, interpolating every van between the two reports
around that instant. Vans therefore move smoothly at 60 fps but only ever
through positions they actually reported; gaps over 4 s (lost signal) jump
instead of gliding.

### 2.11 Python SDK and examples

`python_sdk/gateway_client` (standard library only) provides `GatewayClient`:
`send_telemetry` over HTTP, `subscribe_stream` and `replay` over the binary
protocol, and `disconnect`. `subscribe_stream` saves each record's
`next_offset` to an optional offset file (write-then-rename) after the
callback returns, then sends `ACK`; on reconnect it resumes from that file,
which gives at-least-once delivery. Connection errors are retried with
exponential backoff. `examples/` contains a fleet simulator, a
dispatch-monitor consumer, and a single-vehicle replay tool.

## 3. Load Testing and Container Support

The project includes a Docker build, a raw TCP connection-ramp test, a real HTTP
integration suite, and a staged pooled-HTTP load/shutdown/recovery test.

`docker/Dockerfile` builds the C++ gateway inside Ubuntu 24.04. The Compose
configuration starts the gateway with multiple workers and a Python load-test
container beside it. The load generator opens many TCP clients, sends a known
payload, verifies the response, and keeps successful connections open for a
configured duration.

The test environment raises file-descriptor limits because a connection test is
limited by both application behavior and operating-system resource limits.
`scripts/tune_limits.sh` documents useful host settings, while Compose applies
container-level `nofile` limits.

The raw TCP test exposes connection leaks, accept failures, and unexpected
closes while holding sockets open. `scripts/run_backend_tests.py` adds stages
of 100, 500, 1,000, 2,500, 5,000, 10,000, 20,000, 30,000, and 50,000
concurrent HTTP clients. It samples metrics, triggers graceful shutdown with
work in flight, restarts the gateway against stage-specific storage, and
verifies every acknowledged write. Raw data is kept under ignored
`build/backend-test-results/`; the measured report is `docs/tests.md`. Since
telemetry validation was added, the stage harness sends valid TelemetryEvent
JSON (a unique `vehicle_id` per request); the recorded stage results predate
that change.

`tests/benchmarks/run_bench.sh` measures sustained throughput instead of
burst capacity. For each combination of pool size, body size and connection
count it starts a fresh gateway on an empty log, pins the gateway and `wrk`
to separate CPU sets, warms up for 2 s, and records the median of three 8 s
runs (requests/s and P50/P90/P99). `tests/benchmarks/telemetry.lua` makes
every request a valid TelemetryEvent, padding larger bodies with an ignored
field. `cache_vs_disk_bench` times recent-cache reads against `read_from`.
Results and analysis are in [results.md](results.md) §6.

The Final Testing Plan established pass criteria before each stage run:
zero connection/send/response failures, zero `429`/`503` responses, zero parse
errors, exit code 0 on graceful shutdown, and 100% recovery of acknowledged
writes. Every stage is also required to successfully restart the gateway against
the same persistent storage. All nine stages satisfied these criteria on the
recorded run (2026-10-18).

Each stage uses eight `SO_REUSEPORT` epoll workers, a shared 16-thread request
pool, one shared task queue, and one completion queue per epoll worker. The
shared task queue has no per-epoll-worker depth. Its length and high-water mark
are aggregate metrics, while completion queue depth and high-water are labeled
by worker. The test raises the rate-limit allowance and container file/port
limits so they do not prematurely cap the load. Its `/test/hold` handler is
registered only when `GATEWAY_TEST_MODE` is set; normal runs do not expose it.

## 4. HTTP Request Processing

The current protocol layer adds these types:

```cpp
struct HttpRequest {
    std::string method;
    std::string path;
  std::string query;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};
```

### 4.1 Incremental parser

`HTTPParser` receives whichever bytes have arrived so far through `feed()`. It
never assumes that one socket read contains a complete request.

The parser returns:

- `Incomplete`: more header or body bytes are needed.
- `Complete`: headers are parsed and the full `Content-Length` body is buffered.
- `Error`: the request is malformed, unsupported, or exceeds a configured
  limit.

It waits for the `\r\n\r\n` header terminator, parses the request line and
headers, normalizes header names to lowercase, and uses `Content-Length` to
know how many body bytes are required. It rejects conflicting content lengths,
invalid lengths, unsupported `Transfer-Encoding`, unsupported HTTP versions,
and oversized headers or bodies.

An incremental parser was chosen because TCP does not preserve application
message boundaries. A request can be split across packets, and multiple pieces
can arrive at different times. Routing before the parser reports `Complete`
would risk handling an incomplete request.

Chunked transfer encoding is currently rejected rather than partially
implemented. Explicitly rejecting an unsupported framing mode is safer than
interpreting it as a normal body and producing an incorrect request.

### 4.2 Epoll integration

The read callback in `TCPServer` follows this sequence:

```text
read until EAGAIN
    -> append bytes to the connection's inbound buffer
    -> feed bytes to HTTPParser
    -> wait if status is Incomplete
    -> create a 400 response if status is Error
    -> submit a copy of the complete request to the bounded pool
    -> route and serialize in a pool worker
    -> post completion through the owning loop's eventfd
    -> validate connection generation and queue response on epoll thread
    -> send through the existing nonblocking output path
```

The existing outbound queue and partial-write logic are reused for HTTP
responses. The socket layer does not need to know whether queued bytes came
from an echo, an HTTP response, or a future message type; it only sends bytes
safely and handles backpressure.

The server currently handles one request per connection and closes after the
response. This keeps the initial HTTP integration small and avoids adding
keep-alive and pipelining state before those behaviors are required.

### 4.3 Responses

`HttpResponse` contains a status code, reason phrase, headers, and body.
`to_string()` produces an HTTP/1.1 status line, serializes headers, adds a
`Content-Length` header when the caller did not provide one, adds
`Connection: close`, and appends the body.

Keeping serialization in `HttpResponse` separates HTTP formatting from socket
writing. Handlers can create a response without knowing anything about epoll or
partial sends.

### 4.4 Routing and observability endpoints

`Router` maps an exact `(method, path)` pair to:

```cpp
std::function<HttpResponse(const HttpRequest&)>
```

A matching handler receives the parsed request and returns a response. An
unknown route returns `404 Not Found`.

Exact matching was chosen for the first version because it is predictable and
small. Prefix routes, parameters, middleware, authentication, and method
fallbacks can be added later without changing the parser or socket layer.

The executable currently registers these endpoints:

- `GET /health` returns `{"status":"ok"}` as a JSON liveness response.
- `GET /connections` returns the active connection count as JSON, such as
  `{"connections":3}`.
- `GET /metrics` returns Prometheus text for `gateway_connections_active`,
  `gateway_connections_accepted_total`, `gateway_http_requests_total`,
  `gateway_http_requests_blocked_total`, `gateway_http_parse_errors_total`,
  `gateway_http_responses_total` grouped by status class,
  `gateway_http_request_duration_seconds`, `process_uptime_seconds`,
  `gateway_thread_pool_workers`, `gateway_thread_pool_busy_threads`, shared
  task-queue length/high-water, per-worker accepted/request/completion counters,
  per-worker completion-queue depth/high-water, and `gateway_log_disk_bytes`.
  The latter is a gauge of provisioned segment-file bytes because segments are
  pre-sized. The fleet work added `gateway_telemetry_events_total`,
  `gateway_telemetry_rejected_total`, `gateway_active_vehicles`,
  `gateway_vehicles{status=...}`, `gateway_log_end_offset`,
  `gateway_consumers_connected`, `gateway_consumer_records_sent_total`, and
  `gateway_consumer_lag_bytes`. Consumer connections are counted by the
  consumer gauge, not by `gateway_connections_active`.
- `GET /recent` returns the configured number of newest messages from the
  in-memory circular cache as JSON.
- `GET /large-response` returns a deterministic 512 KiB body for transport
  tests of slow readers and partial writes.
- `POST /publish` validates a TelemetryEvent, appends it, updates
  `FleetState`, and returns `{"offset":N}`.
- `GET /vehicles`, `GET /consumers`, and the dashboard routes are described
  in [protocol.md](protocol.md).
- `GET /test/hold` sleeps for 15 seconds and is registered only when
  `GATEWAY_TEST_MODE` is set for shutdown testing.

The metrics collector counts each complete parsed request immediately before
router dispatch; malformed requests are counted separately. Rate-limited
requests are included in both request and blocked-request totals. A `/metrics`
scrape counts as a request and active connection, while its response class and
duration are recorded after the exposition is generated. Request duration
covers pool execution, routing, and serialization; it excludes queue wait,
request parsing, and socket transmission.

Each worker still owns its own `ConnectionManager`, so the connection count is
maintained separately as an atomic counter on `TCPServer`. The counter is
incremented after a client is accepted and decremented when that client is
removed. This provides one safe aggregate value without reading another
worker's map or adding locks to the normal connection path.

`POST /publish` accepts only valid TelemetryEvent JSON; consumers receive the
stored records over the binary protocol (see [architecture.md](architecture.md)).

The large-response endpoint is intentionally simple and deterministic. It is a
transport test surface, not a telemetry feature: the body consists of repeated
`x` bytes so a raw-socket client can verify the exact `Content-Length` and
payload while reading slowly.

The router also owns a shared per-IP token-bucket limiter. The IP map uses a
`shared_mutex` for existing-entry lookup and exclusive insertion; each bucket
has its own mutex because consuming a token mutates its state. Defaults allow a
burst of 10 requests and refill at 5 requests per second. Both values come from
`Config` and can be overridden in the config file or CLI.
When a bucket is empty, routing returns `429 Too Many Requests` before the
application handler is called.

`TokenBucket::consume()` measures elapsed time with `steady_clock`, adds tokens
at the configured refill rate up to the bucket capacity, and deducts one token
when available. The bucket accepts a clock function that defaults to
`steady_clock::now()` in production. Unit tests inject a fake clock and advance
it directly to verify burst allowance, refill, capacity capping, and zero-rate
behavior without sleeping or opening network connections.

## 4.5 HTTP integration test strategy

The repository includes a separate Docker-based raw-socket integration suite
in `tests/http_integration_test.py`. It uses Python's standard-library
`socket`, not an HTTP client library, so the test controls the exact TCP writes
and can deliberately send a request one byte at a time.

Run it later with:

```bash
./scripts/run_http_integration_test.sh
```

The Compose file starts the compiled gateway in one container and the Python
test in a separate `python:3.12-alpine` container. The test container exits
with success only when every case passes. Docker is used here for reproducible
service startup, clean networking, and a test environment separate from the
developer's host Python installation.

The suite covers:

- A normal `GET /health` request and response framing.
- A byte-by-byte `GET /health` request with delays between writes.
- A fragmented request body with `Content-Length`.
- Malformed request lines, headers, HTTP versions, and transfer encoding.
- Header normalization, repeated headers, and conflicting content lengths.
- Header and body size-limit rejection.
- Exact method/path routing and 404 responses.
- Connection closure after one response.
- 100 concurrent raw-socket HTTP clients.
- A slow reader consuming a 512 KiB response in small chunks.
- `/recent` empty-cache JSON and query validation, plus `/metrics` exposition.
- `/publish` empty-body rejection and visibility of a stored telemetry event.

The last case uses `GET /large-response`, a deterministic transport-test
endpoint that returns 512 KiB of `x` bytes. The client limits its receive
buffer, reads slowly, verifies the exact `Content-Length`, and checks every
body byte. This gives the server's outbound queue and `EPOLLOUT` path a real
backpressure scenario. It validates complete delivery, not a particular number
of individual `send()` calls, because TCP and the kernel are free to combine or
split writes.

These tests complement the other test layers:

```text
C++ unit tests (51 CTest cases)  -> parser, limiter, pool, config, metrics, log,
                                    cache, telemetry validator, fleet state, frame codec
raw-socket HTTP integration      -> real TCP server, parser, router, responses
slow-reader integration          -> partial writes and EPOLLOUT backpressure
pytest integration (33 cases)    -> real gateway process: publish validation,
                                    fleet view, SDK replay/streaming, consumer
                                    resume, dashboard whitelist
pytest chaos (2 cases)           -> kill -9 durability, SIGTERM drain under load
raw TCP connection-ramp test     -> connection retention
backend HTTP stage test          -> concurrency, shutdown drain, and recovery
wrk benchmark harness            -> sustained throughput and latency percentiles
```

The pytest suites live in `tests/integration` and `tests/chaos`;
`tests/conftest.py` starts a gateway on free local ports with its own
storage directory and can stop, `kill -9`, and restart it. CI runs the C++
and pytest suites in Release and again under AddressSanitizer +
UndefinedBehaviorSanitizer.

Focused C++ tests also cover config-file/CLI precedence, pool queue capacity and
drain semantics, exception survival, concurrent same-IP rate limiting, and a
blocked handler completing during the server's SIGTERM shutdown path. Run the
compiled test suite with the command below. Each test function is registered
as an individual CTest case, so CTest reports its name and pass/fail status
separately rather than reporting only one result per test executable:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The latest Docker HTTP integration run passed all 13 cases. Its metrics check
verifies `gateway_thread_pool_workers`, `gateway_thread_pool_busy_threads`,
shared queue length/high-water, per-worker metrics, and
`gateway_log_disk_bytes`. It also verifies an empty cache and `/recent` query
validation for zero, malformed, unknown, repeated, and over-limit values.

`mmap_log_tests` verifies restart persistence, variable and empty payloads,
segment rollover and global offsets, exact-fit rollover, oversized-record
rejection, incomplete-header and incomplete-payload recovery, payload bytes
written before a missing header, active-tail checksum rejection, and fatal
corruption in a sealed segment. `circular_buffer_tests` verifies chronological
ordering below capacity, exactly at capacity, after wraparound, zero and
oversized reads, and capacity one. `http_parser_tests` verifies that a query
string is separated from the route path. `tcp_server_drain_tests` seeds messages
through the server API and checks default ordering, a limited count, zero count,
invalid and over-limit counts, JSON escaping, response content type, configured
segment-size metrics, and graceful response draining. `metrics_tests` checks
that the disk gauge's help text describes provisioned bytes.
`telemetry_event_tests`, `fleet_state_tests` and `frame_codec_tests` cover
the validator, the fleet view (with a fake clock and an 8-thread concurrency
case) and the consumer framing; `mmap_log_tests` also covers `read_from` and
the CRC golden check. Per-case details are in [results.md](results.md) §2.2
and §3.3.

The Docker integration suite verifies that `/publish` rejects an empty body and
stores a valid telemetry event. Field-level validation is covered by
`telemetry_event_tests` and `tests/integration/test_telemetry_publish.py`.

Run the staged backend test with:

```bash
python3 scripts/run_backend_tests.py
```

Set `BACKEND_TEST_STAGES=100,500` to select stages. The runner stores raw
per-stage JSONL in `build/backend-test-results/stage-results.jsonl`. Its own
`/metrics` probes are included in server counters, so aggregate counts may be a
little higher than the load-generator cohort. See [tests.md](tests.md) for
measured results and caveats. The Final Testing Plan recorded in `docs/tests.md`
established pass criteria before running (zero failures, zero error responses,
clean exit, 100% recovery) and documented environment parameters (host
hardware, Docker limits, ephemeral-port range, rate-limit override rationale).
The latest run (2026-10-18) passed all nine configured stages through 50,000
concurrent clients. Each stage connected the full cohort, sent one test-only
held request plus opaque writes for the other clients, initiated SIGTERM while
work was in flight, verified the in-flight count at signal time, restarted with
the same stage-specific log, and verified every acknowledged write.
Per-stage recovery and shutdown passed at all nine stages.

## 5. Telemetry Contract

Telemetry is sent as JSON in the body of `POST /publish`. The
documented event shape is defined in [protocol.md](protocol.md):

```text
TelemetryEvent {
  vehicle_id: string
  lat: number
  lng: number
  speed: number
  status: "moving" | "idle" | "offline"
  timestamp: integer
}
```

`POST /publish` parses the body with `parse_telemetry`
(`src/telemetry/telemetry_event.cpp`): a flat JSON object, all six fields
required with the types and ranges in [protocol.md](protocol.md), unknown
scalar fields ignored. Valid bodies are appended byte-for-byte; invalid ones
get `400 {"error":"<reason>"}` and never reach the log. HTTP framing stays in
`HTTPParser`; the meaning of a record stays in the telemetry layer.

No additional telemetry endpoint is needed. `POST /publish` is the intended
application contract.

The protocol boundary is intentional: producers speak HTTP to this parser,
while consumers use a separate binary TCP protocol (`src/protocol/`). Consumers
do not speak HTTP. Keeping those protocols separate allows the producer-facing API
and consumer-facing stream format to evolve independently.

## 6. Why This Overall Approach Was Chosen

The implementation favors a small number of clear ownership boundaries:

- Socket setup and worker lifecycle belong to `TCPServer`.
- Readiness notification belongs to `EpollLoop`.
- Per-client state belongs to `ConnectionManager` and `Connection`.
- HTTP framing belongs to `HTTPParser`.
- HTTP formatting belongs to `HttpResponse`.
- Application dispatch belongs to `Router` and route handlers.
- Telemetry validation belongs to `parse_telemetry`; the derived fleet view
  belongs to `FleetState`.
- Consumer framing belongs to `FrameDecoder`; replay reads belong to
  `MemoryMappedLog::read_from`.

This structure was chosen for four reasons:

1. **Correctness:** TCP fragmentation and partial writes are handled explicitly.
2. **Scalability:** nonblocking epoll and per-worker ownership avoid one thread
   per client and minimize lock contention.
3. **Safety:** size limits, error responses, cleanup, and bounded queues protect
   the process from malformed input and slow peers.
4. **Extensibility:** protocol parsing, routing, and telemetry processing can
   evolve without rewriting the operating-system event loop.

The project therefore grows from a tested network foundation toward a telemetry
service instead of combining untested networking, parsing, and business logic
in one callback.

## 7. Current Scope and Next Work

Implemented today:

- CMake/C++17 build with compiler warnings, defaulting to Release.
- Config-file and CLI runtime settings for port, backlog, epoll workers, pool
  size, queue capacity, log path, rate limiting, storage, consumer port and
  cap, dashboard directory, and offline window (`--help` prints them).
- IPv4 TCP listener setup and cleanup.
- Centralized timestamped logging.
- Nonblocking sockets and `EAGAIN` handling.
- Linux epoll event loop.
- Per-worker connection management.
- `SO_REUSEPORT` worker scaling.
- Bounded shared request pool with eventfd completions returned to connection owners.
- Partial-write handling and bounded outbound buffering.
- SIGINT/SIGTERM graceful draining of accepted tasks and client cleanup.
- Docker build and concurrent TCP connection-ramp testing.
- Incremental HTTP request parsing.
- HTTP response serialization.
- Exact method/path routing.
- JSON `GET /health` and aggregate `GET /connections` endpoints.
- Per-IP token-bucket rate limiting with `429 Too Many Requests` responses.
- Atomic process-wide active-connection accounting across workers.
- Parser unit tests and live health/404 checks.
- Docker raw-socket HTTP integration tests for complete, fragmented, malformed,
  concurrent, size-limit, routing, connection-close, slow-reader, and
  publish/recent/metrics cases (13 cases total).
- Focused C++ rate-limiter tests for burst/refill behavior, per-IP isolation,
  and handler bypass on `429 Too Many Requests`.
- Config, pool saturation/drain, concurrent limiter, and server shutdown tests.
- Pool worker, busy-thread, shared queue length/high-water, and per-epoll-worker
  request/completion-queue metrics.
- Segmented mmap log with CRC-checked records, offset tracking, and startup recovery.
- Recent-message cache and `GET /recent`; log capacity gauge in `/metrics`.
- Storage recovery, rollover, circular-cache, and recent-route test coverage.
- A deterministic large-response endpoint for testing partial response writes.
- `POST /publish` persistence route (validated telemetry since the fleet work) and opt-in shutdown hold route.
- Staged HTTP concurrency, graceful-shutdown, and persistence-recovery tests
  (100 through 50,000 concurrent clients; 9 stages, all passed).
- Final Testing Plan with pre-run pass criteria, environment documentation,
  per-stage shutdown and persistence verification, and per-worker metrics.
- Telemetry event documentation.
- Comprehensive test report in `docs/tests.md` and results summary in `docs/results.md`.

Added by the fleet work (see [architecture.md](architecture.md)):

- TelemetryEvent validation on `POST /publish`, `FleetState` and `GET /vehicles`.
- Binary consumer protocol with replay (`read_from`), live tailing, ACKs,
  backpressure, `GET /consumers`, and consumer metrics.
- Dashboard served by the gateway; Python SDK; simulator, consumer and replay
  examples.
- pytest integration and chaos suites, `wrk` benchmark harness, CI.
- Table-driven CRC32 and a Release default build (see [journal.md](journal.md)).

Still to implement:

- Authentication and TLS; broader spam/DDoS protection.
- Inactive-IP bucket expiration and a bounded limiter map.
- Asynchronous sync of sealed segments so rollover does not stall appends.
- Log retention and consumer groups.
- Keep-alive or pipelining, and chunked transfer encoding, if required.
