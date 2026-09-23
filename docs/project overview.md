# Telemetry Gateway: Project Overview

## 1. Project Purpose

Telemetry Gateway is a Linux C++ service for receiving telemetry from delivery
vehicles and connected devices. The long-term system will accept producer data,
validate it, preserve it, and make it available to downstream consumers.

### Technical view

The current implementation is an event-driven TCP server using C++17,
non-blocking IPv4 sockets, Linux `epoll`, multiple `SO_REUSEPORT` workers,
per-connection state, bounded output queues, incremental HTTP parsing, HTTP
response serialization, and exact method/path routing.

### In simple terms

This is the traffic-handling foundation for a fleet-monitoring system. It can
talk to many clients at the same time, understand basic HTTP requests, and
send each request to the correct handler.

## 2. Current Status

The project has been built in layers:

1. TCP socket setup and cleanup.
2. Non-blocking I/O and Linux `epoll` event handling.
3. Per-worker connection management and bounded output buffering.
4. Graceful shutdown, logging, and Docker-based connection testing.
5. Incremental HTTP request parsing and HTTP response serialization.
6. Exact method/path routing and observability endpoints.

The networking and HTTP transport layers are implemented. Telemetry business
logic is not complete: the HTTP body is assembled as text, but JSON validation,
storage, and downstream delivery still need to be added.

### Technical view

The server currently handles one HTTP request per connection and closes the
connection after the response. The parser supports HTTP/1.0 and HTTP/1.1
request lines, lowercase header names, `Content-Length`, fragmented input,
header/body limits, and clear parse errors. It rejects unsupported
`Transfer-Encoding` and conflicting content lengths.

### In simple terms

The roads, traffic lights, and mailroom are working. The mailroom can receive
an HTTP package and check its size and format, but the part that understands
whether a vehicle's location and status are valid is still being built.

## 3. Protocol Architecture

The gateway has two distinct protocol roles:

- **Producers**, such as vehicles and SDK clients, send telemetry over HTTP.
  The intended application request is `POST /publish` with JSON in the body.
- **The gateway** parses the HTTP message, validates its framing and limits,
  then routes a complete request to an application handler.
- **Consumers**, planned for Week 9, will receive telemetry through a separate
  binary TCP protocol. They will not use the producer-facing HTTP parser.

HTTP was chosen and implemented for the producer side because it is a clear,
readable, widely supported request format. The future binary TCP protocol is a
separate consumer-facing stream because consumers may need compact messages,
lower overhead, and efficient continuous delivery.

### Technical view

The protocol boundary is an ownership boundary. HTTP framing belongs to
`HTTPParser`; HTTP formatting belongs to `HttpResponse`; application dispatch
belongs to `Router`; future telemetry validation and persistence belong to an
application layer; and the binary consumer stream will have its own protocol
implementation.

### In simple terms

Think of the gateway as a building with two entrances. Vehicles and SDKs use
the normal front entrance, HTTP. Future consumers use a separate high-speed
entrance, binary TCP. Both use the same reliable building infrastructure, but
they do not have to understand each other's entrance.

## 4. Building and Running

From the repository root:

```bash
cmake -S . -B build
cmake --build build
./build/message_gateway [port] [backlog] [workers]
```

Defaults are port `8080`, backlog `128`, and one worker per detected hardware
thread. For example:

```bash
./build/message_gateway 9000 256 2
```

The server stops cleanly when it receives `Ctrl+C`.

### Technical view

`main.cpp` validates the optional numeric arguments, registers routes, creates
`TCPServer`, and starts the worker threads. A worker-count value of `0` selects
the hardware-concurrency count, with at least one worker.

### In simple terms

The first commands compile the program. The last command starts it. The three
optional numbers choose the listening port, how many connections can wait in
the kernel backlog, and how many server teams should run.

## 5. Startup and Worker Architecture

The startup path is:

1. Parse and validate command-line arguments.
2. Start the configured number of worker threads.
3. Each worker creates an IPv4 TCP listener.
4. Enable `SO_REUSEADDR` and `SO_REUSEPORT`.
5. Bind, listen, and set the listener to non-blocking mode.
6. Register the listener with that worker's `EpollLoop`.

Each worker owns one listener, one event loop, and one `ConnectionManager`.
Linux distributes new connections among the reuse-port listeners. Once a
connection is accepted, it remains with that worker.

```text
Worker 0: listener -> EpollLoop -> ConnectionManager -> clients
Worker 1: listener -> EpollLoop -> ConnectionManager -> clients
Worker N: listener -> EpollLoop -> ConnectionManager -> clients
```

### Technical view

Worker ownership keeps connection maps and buffers thread-confined. The normal
read/write path therefore does not need a lock for every client operation.
Shared shutdown and active-connection state use atomics.

### In simple terms

The server creates several teams. Each team has its own waiting line, activity
monitor, and client list. A client does not get passed between teams, which
keeps the work simpler and safer.

## 6. Non-blocking I/O and `epoll`

Blocking I/O could leave a worker waiting for one slow client. The server sets
listeners and client sockets to `O_NONBLOCK` and treats `EAGAIN` and
`EWOULDBLOCK` as normal "no more work right now" results. `EINTR` is retried.

`EpollLoop` registers descriptors and waits for readiness events. Client
sockets use `EPOLLIN` for readable data and `EPOLLRDHUP` for a peer half-close.
`EPOLLOUT` is enabled only when queued output could not be sent immediately.

### Technical view

The loop is level-triggered. A ready descriptor is drained until `read()` or
`accept()` returns `EAGAIN`. This avoids busy-polling inactive clients and lets
one worker manage many connections without one blocking thread per client.

### In simple terms

The server does not repeatedly ask every client whether it has something to
say. The operating system points it to clients that need attention, so a quiet
client does not hold everyone else up.

## 7. Connection State and Request Handling

Each connection stores its file descriptor, an inbound buffer, an
`HTTPParser`, an outbound buffer, an outbound offset, and peer-close state.

The read path is:

1. Read available bytes until the socket returns `EAGAIN`.
2. Feed each chunk to the connection's incremental `HTTPParser`.
3. Wait when the parser returns `Incomplete`.
4. Create `400 Bad Request` when parsing fails.
5. Route only after the parser returns `Complete`.
6. Serialize the resulting `HttpResponse`.
7. Queue and flush the response using non-blocking writes.

TCP is a byte stream, so one HTTP request may arrive in many reads. The parser
waits for `\r\n\r\n`, parses the request line and headers, reads
`Content-Length`, and then waits for the complete body. It does not assume
that a socket read equals one HTTP request.

### Technical view

The server currently processes one request per connection. When `send()` is
partial or returns `EAGAIN`, the unsent suffix remains in the outbound buffer;
the offset identifies the next byte to send. Once the response is flushed, the
connection is closed.

### In simple terms

A request can arrive in small pieces. The server collects all the pieces,
checks that the package is complete, finds the right handler, and sends one
answer. It never treats half a request as a finished request.

## 8. HTTP Parser and Safety Limits

`HTTPParser` has configurable defaults of 16 KiB for headers and 1 MiB for the
body. These values are byte limits, not bit limits.

The parser:

- Accepts HTTP/1.0 and HTTP/1.1 request lines.
- Stores the method and path.
- Normalizes header names to lowercase.
- Trims spaces and tabs around header values.
- Validates numeric `Content-Length` values.
- Rejects conflicting `Content-Length` headers.
- Rejects unsupported `Transfer-Encoding`.
- Rejects malformed request lines and headers.
- Rejects requests exceeding configured header or body limits.

### Technical view

The parser returns `Incomplete`, `Complete`, or `Error`. This explicit state
machine prevents routing before enough bytes have arrived and bounds memory
used for a single request.

### In simple terms

The parser is a careful receptionist. It waits until all pages of a request
arrive, checks that the envelope is valid and not enormous, and refuses a
request it cannot safely understand.

## 9. Routing and Responses

`Router` maps an exact `(method, path)` pair to a handler:

```cpp
std::function<HttpResponse(const HttpRequest&)>
```

Unknown routes return `404 Not Found`. `HttpResponse::to_string()` writes the
HTTP status line, headers, an automatic `Content-Length` when needed,
`Connection: close`, and the response body.

The executable currently registers:

- `GET /health` -> `{"status":"ok"}`.
- `GET /connections` -> the aggregate active connection count as JSON.

The active count is atomic because each worker owns a separate connection map.

### Technical view

Exact matching was chosen for predictability. Route handlers do not know about
`epoll` or partial writes; they return an `HttpResponse`, leaving transport
serialization and delivery to the server layer.

### In simple terms

The router is a receptionist with a lookup list. It checks the method and path,
hands the request to the matching handler, or says "not found." The response
object packages the answer in the format HTTP clients expect.

## 10. Backpressure, Memory, and Cleanup

Each connection has a 1 MiB outbound queue limit. If a slow receiver causes the
queue to exceed that limit, the connection is closed rather than allowing one
client to consume unbounded memory. Partial writes are resumed with `EPOLLOUT`.

When a connection closes or fails, the server removes it from `epoll`, erases
its connection state, decrements the atomic active count, and closes the file
descriptor. `SIGINT` sets an atomic shutdown flag; workers leave their loops,
listeners close, and `TCPServer` joins the worker threads.

### Technical view

Resource ownership is explicit and RAII-oriented. Error paths close descriptors,
and unusually large drained buffers are released rather than retained forever.
Logging is centralized and protected against interleaved worker output.

### In simple terms

If someone receives data too slowly, the server keeps only a reasonable amount
for them and disconnects them before they consume all available memory. When a
client leaves, every related resource is cleaned up.

## 11. Per-IP Rate Limiting

The gateway uses a token bucket to protect the request path from one
malfunctioning or spoofed device flooding the ingestion pipe with fake pings.
Each client IP gets its own bucket with a capacity of 10 requests and a refill
rate of 5 requests per second.

### Technical view

`TokenBucket::consume()` uses `steady_clock` to calculate elapsed time, refills
tokens up to capacity, and consumes one token when available. `RateLimiter`
stores buckets in an `unordered_map<string, TokenBucket>` protected by a mutex
because multiple `SO_REUSEPORT` workers share the router's limiter. The router
checks the client IP after parsing and before handler dispatch. An empty bucket
produces `429 Too Many Requests`, and the handler is not called.

The process accepts `RATE_LIMIT_CAPACITY` and `RATE_LIMIT_REFILL_RATE` startup
environment variables for controlled test configurations. The Docker HTTP
integration suite raises the capacity so its unrelated transport cases do not
consume the production bucket; focused C++ tests verify the default rejection
policy directly.

### In simple terms

Every device gets a small cup of request tickets. It may spend the tickets in a
short burst, but new tickets slowly appear over time. If the cup is empty, the
gateway says “try later” instead of doing the requested work. A noisy device
does not use up everyone else's tickets.

## 12. Testing and Verification

The repository includes CMake/CTest parser unit tests in
`tests/http_parser_test.cpp`. They cover fragmented headers and bodies,
requests without bodies, malformed requests, conflicting lengths, unsupported
chunked transfer, and size limits.

The repository also includes a Docker-based raw-socket HTTP integration suite
in `tests/http_integration_test.py`. It uses Python's standard-library
`socket`, not `requests`, and tests the real compiled gateway through the
container network.

The suite covers:

- Normal `GET /health` and response framing.
- Byte-by-byte request delivery with small delays.
- Fragmented request bodies using `Content-Length`.
- Malformed request lines, headers, HTTP versions, and transfer encoding.
- Header normalization, repeated headers, and conflicting lengths.
- Header and body size-limit rejection.
- Exact routing and `404 Not Found` responses.
- Per-IP rate limiting and `429 Too Many Requests` responses.
- 100 concurrent HTTP clients.
- Slow-reader delivery of the 512 KiB `/large-response` body.

The command used was:

```bash
./scripts/run_http_integration_test.sh
```

The observed result was:

```text
PASS all HTTP integration tests (10)
http-test exited with code 0
```

These results show that the parser assembles tiny TCP fragments, rejects
invalid framing, enforces limits, routes requests correctly, handles multiple
clients, and completes a large response through the outbound queue and
`EPOLLOUT` path. The Compose gateway exit code `137` during teardown is
expected because the test runner stops the gateway after the test container
finishes.

The Docker load test uses `scripts/tcp_load_generator.py` to create many raw
TCP clients, verify an echo payload, hold connections open, and report
failures. Recorded connection-ramp results include a successful 50,000-client
run after increasing the load-generator container's ephemeral source-port
range and file-descriptor limits.

That load test is a historical raw TCP echo test. It does not test the current
HTTP parser or telemetry validation. The raw-socket HTTP integration suite now
covers real partial TCP reads and partial response delivery separately.

### Technical view

Unit tests isolate parser behavior. The HTTP integration suite exercises the
real connection acceptance, epoll, parser, router, response, buffering, and
cleanup path. The 50,000-client Docker test remains focused on raw TCP
connection retention and scale.

### In simple terms

The small tests check individual rules, the HTTP suite checks the real server
with realistic broken and slow clients, and the 50,000-client test checks how
many connections the network foundation can keep alive.

## 13. Implemented, Planned, and Limited

### Implemented

- CMake/C++17 build with strict compiler warnings.
- Configurable TCP port, backlog, and worker count.
- IPv4 sockets, non-blocking mode, `epoll`, and `SO_REUSEPORT` workers.
- Per-worker connection tracking and bounded outbound buffering.
- Partial-write handling and graceful shutdown.
- Incremental HTTP request parsing and HTTP response serialization.
- Header/body limits and malformed-request protection.
- Exact method/path routing and JSON observability endpoints.
- Atomic process-wide active-connection accounting.
- Parser unit tests and Docker TCP connection-ramp testing.
- Protocol documentation describing the HTTP/binary-TCP split.
- Docker raw-socket HTTP integration tests with 10 passing cases.
- Per-IP token-bucket rate limiting with a mutex-protected shared limiter.

### Planned next

- Register and implement `POST /publish`.
- Parse and validate telemetry JSON and field ranges.
- Persist or forward accepted telemetry.
- Implement the Week 9 binary consumer protocol.
- Add authentication and broader spam/DDoS protection.
- Add downstream backpressure and failure-recovery tests.
- Add production-level metrics and observability.

### Current limitations

- Telemetry JSON is not yet validated by an application handler.
- There is no authentication or authorization.
- There is no persistent buffering or downstream broadcasting.
- Keep-alive and pipelining are not implemented; connections close after one response.
- Chunked transfer encoding is rejected.
- The queue limit is a fixed safe default, not workload-specific tuning.
- The Docker load test has not yet been converted from raw echo to HTTP.
- Inactive IP buckets are not yet expired, so the limiter map needs bounded
  cleanup before hostile high-cardinality traffic is considered production-safe.

### Interview summary

**Technical answer:** The project establishes a thread-confined, event-driven
TCP foundation and adds a bounded incremental HTTP transport layer. It has
clear ownership boundaries between sockets, parsing, routing, and future
telemetry processing. The main remaining work is application-level telemetry
validation, storage/forwarding, consumer streaming, and production controls.

**Simple answer:** We first made sure the server can handle many connections
reliably. Then we taught it how to understand HTTP safely. Next we need to
teach it what telemetry means, where to store it, and how to stream it to the
future consumers.

## 14. Keeping This Document Updated

Update this document when a change affects the architecture, a design decision,
the implemented feature list, planned work, or current limitations. Each
update should explain what changed, why it was chosen, and how it supports the
gateway's goals.
