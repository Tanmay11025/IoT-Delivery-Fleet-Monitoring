# Implementation Guide

## 1. Project Purpose

This project is building a Linux-based gateway for delivery-fleet telemetry.
Vehicles and connected devices will send location, speed, status, and timing
data to the gateway. The gateway is intended to accept many connections,
process requests reliably, and later forward valid telemetry to other services.

The project was built in layers. We started with the smallest useful network
program, proved that the operating-system primitives worked, and then added
scalability, safety, HTTP handling, and telemetry-oriented contracts. This
order was chosen because each layer depends on the previous one. It is easier
to debug a reliable TCP foundation before adding parsing and application logic.

## 2. Implementation History

### 2.1 Build and runtime configuration

The gateway is a CMake-based C++17 executable built with `-Wall`, `-Wextra`,
and `-Wpedantic`. `Config` centralizes the listening port, backlog, epoll worker
count, request-pool size and queue capacity, log path, and rate-limit policy.
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

Each epoll worker owns one listener, event loop, and `ConnectionManager`.
Connections contain their parser, inbound/outbound buffers, partial-write
offset, peer-close state, in-flight flag, and generation token. A 1 MiB
per-connection output limit bounds slow-reader memory use. The generation token
prevents a late asynchronous response from being applied to a new connection
that reused the same file descriptor.

### 2.5 Epoll workers and request pool

`SO_REUSEPORT` distributes newly accepted connections among the configured
epoll workers. Those workers keep all connection mutation and socket I/O
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
shutdown begins and drains all accepted tasks. Each epoll loop exits only when
its outstanding tasks and connections are gone; the server then joins the
epoll threads and pool workers. Hard listener errors trigger the same drain
path, while client errors remove the descriptor and close the connection.

## 3. Load Testing and Container Support

The project includes a Docker build and a connection-ramp test setup.

`docker/Dockerfile` builds the C++ gateway inside Ubuntu 24.04. The Compose
configuration starts the gateway with multiple workers and a Python load-test
container beside it. The load generator opens many TCP clients, sends a known
payload, verifies the response, and keeps successful connections open for a
configured duration.

The test environment raises file-descriptor limits because a connection test is
limited by both application behavior and operating-system resource limits.
`scripts/tune_limits.sh` documents useful host settings, while Compose applies
container-level `nofile` limits.

This test design was chosen to expose connection leaks, accept failures,
backpressure problems, and unexpected server closes under sustained concurrent
connections. It is intentionally separate from parser tests: load tests answer
whether the network service remains usable at scale, while unit tests answer
whether protocol logic is correct.

## 4. HTTP Request Processing

The current protocol layer adds these types:

```cpp
struct HttpRequest {
    std::string method;
    std::string path;
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
  `gateway_thread_pool_workers`, `gateway_thread_pool_busy_threads`, and
  `gateway_thread_pool_queue_length`.
- `GET /large-response` returns a deterministic 512 KiB body for transport
  tests of slow readers and partial writes.

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

The telemetry contract reserves `POST /publish` for telemetry JSON, but a
publish handler and JSON validation/storage are application-level work still to
be added.

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

The last case uses `GET /large-response`, a deterministic transport-test
endpoint that returns 512 KiB of `x` bytes. The client limits its receive
buffer, reads slowly, verifies the exact `Content-Length`, and checks every
body byte. This gives the server's outbound queue and `EPOLLOUT` path a real
backpressure scenario. It validates complete delivery, not a particular number
of individual `send()` calls, because TCP and the kernel are free to combine or
split writes.

These tests complement the other test layers:

```text
HTTPParser C++ unit tests       -> parser logic in isolation
raw-socket HTTP integration     -> real TCP server, parser, router, responses
slow-reader integration         -> partial writes and EPOLLOUT backpressure
50,000-client Docker load test  -> connection-scale stability and retention
```

Focused C++ tests also cover config-file/CLI precedence, pool queue capacity and
drain semantics, exception survival, concurrent same-IP rate limiting, and a
blocked handler completing during the server's SIGTERM shutdown path. Run the
compiled test suite with:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The HTTP integration metrics check verifies `gateway_thread_pool_workers`,
`gateway_thread_pool_busy_threads`, and `gateway_thread_pool_queue_length`.

The suite intentionally does not test telemetry JSON validation yet because
`POST /publish` has not been implemented. Once that handler exists, the same
Docker test should add valid and invalid telemetry requests and assert the
application-level response.

## 5. Telemetry Contract

Telemetry is sent as JSON in the body of `POST /publish`. The documented event
shape is defined in [protocol.md](protocol.md):

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

The HTTP transport currently treats the body as an opaque string. This is
intentional separation of responsibilities: the transport assembles a complete
body, while the future publish handler will validate the JSON, apply telemetry
rules, store or forward the event, and produce the application response.

No additional telemetry endpoint is needed. `POST /publish` is the intended
application contract.

The protocol boundary is intentional: producers speak HTTP to this parser,
while Week 9 consumers will use a separate binary TCP protocol. Consumers will
not speak HTTP. Keeping those protocols separate allows the producer-facing API
and consumer-facing stream format to evolve independently.

## 6. Why This Overall Approach Was Chosen

The implementation favors a small number of clear ownership boundaries:

- Socket setup and worker lifecycle belong to `TCPServer`.
- Readiness notification belongs to `EpollLoop`.
- Per-client state belongs to `ConnectionManager` and `Connection`.
- HTTP framing belongs to `HTTPParser`.
- HTTP formatting belongs to `HttpResponse`.
- Application dispatch belongs to `Router` and route handlers.
- Telemetry validation and persistence belong to the future application layer.

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

- CMake/C++17 build with compiler warnings.
- Config-file and CLI runtime settings for port, backlog, epoll workers, pool
  size, queue capacity, log path, and rate limiting.
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
  concurrent, size-limit, routing, connection-close, and slow-reader cases.
- Focused C++ rate-limiter tests for burst/refill behavior, per-IP isolation,
  and handler bypass on `429 Too Many Requests`.
- Config, pool saturation/drain, concurrent limiter, and server shutdown tests.
- Pool worker, busy-thread, and queue-length metrics.
- A deterministic large-response endpoint for testing partial response writes.
- Telemetry event documentation.

Still to implement:

- Register the `POST /publish` handler.
- Validate telemetry JSON and its field ranges.
- Persist or forward accepted telemetry.
- Add authentication, rate limiting, and spam/DDoS protection.
- Add keep-alive or pipelining if required.
- Add chunked transfer encoding if required.
- Add application-level telemetry and failure tests.
- Add inactive-IP bucket expiration and a bounded limiter map.
- Replace the transport-only HTTP integration fixtures with telemetry-aware
    tests once `POST /publish` is implemented.
