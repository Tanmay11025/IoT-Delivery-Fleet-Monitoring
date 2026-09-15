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

### 2.1 Repository and build foundation

The project began with a CMake-based C++17 executable. The build uses strict
compiler warnings:

```text
-Wall -Wextra -Wpedantic
```

`main.cpp` is the application entry point. CMake keeps compilation explicit,
which makes the list of production files and test targets visible and makes the
project easy to build locally or inside Docker.

The command-line configuration supports:

```text
message_gateway [port] [backlog] [workers]
```

Arguments are parsed and validated before the server starts. Invalid ports,
backlogs, worker counts, or extra arguments are rejected early rather than
allowing a partially configured server to run.

### 2.2 TCP server and socket lifecycle

The first working server established the normal TCP lifecycle:

```text
socket -> setsockopt -> bind -> listen -> accept -> read/send -> close
```

The server creates an IPv4 TCP listening socket, binds it to the configured
port, starts listening, accepts client connections, and closes resources during
normal shutdown or failure.

Socket ownership was then moved into the `TCPServer` abstraction. Cleanup is
centralized in destructors and close helpers so that error paths do not leave
file descriptors open. This RAII-style ownership was chosen because sockets are
operating-system resources and manual cleanup scattered across many branches
is easy to get wrong.

### 2.3 Centralized logging

`src/core/logger.h` provides timestamped logging for server events. Logging is
centralized so worker startup and other operational messages use one format.
Output is protected from interleaving between worker threads.

The server does not log every byte or every connection by default. That choice
keeps logging from becoming a performance bottleneck during connection ramps.

### 2.4 Nonblocking I/O

Listening and client sockets are configured with `O_NONBLOCK`. A blocking
socket could make a worker wait indefinitely for one slow client. A nonblocking
socket returns immediately when no operation can currently proceed.

The implementation treats `EAGAIN` and `EWOULDBLOCK` as normal flow-control
signals, not as fatal errors. The event loop can then move on to other clients.
`EINTR` is retried because an interrupted system call is not a connection
failure by itself.

This approach was chosen because the gateway is expected to keep many network
connections open at the same time. It prevents one idle or slow connection from
stopping progress for the rest of the worker.

### 2.5 Epoll event loop

`EpollLoop` wraps Linux `epoll`. It registers file descriptors, waits for
readable or writable events, modifies interest masks, and removes descriptors
when they close.

The loop uses level-triggered events. It processes a ready descriptor until
`read()` or `accept()` returns `EAGAIN`, which ensures that all currently
available work is drained before the loop waits again.

The listener watches `EPOLLIN`. Client sockets watch `EPOLLIN` and
`EPOLLRDHUP`; `EPOLLOUT` is enabled only when queued output cannot be sent
immediately. This avoids waking the event loop for writable sockets that have
nothing to send.

`epoll` was chosen instead of polling every connection or creating one blocking
thread per client because the kernel reports only descriptors with activity.
That reduces wasted CPU and gives one worker a low-memory way to manage many
connections.

### 2.6 Connection management

`ConnectionManager` stores active connections in a map keyed by file descriptor.
Each worker owns its own manager. A `Connection` currently contains:

```text
file descriptor
HTTP parser
inbound buffer
outbound buffer
outbound offset
peer-closed state
```

Keeping state per connection is necessary because TCP is a byte stream. One
request may arrive in many reads, and one read may contain more data than the
server can process immediately.

The outbound buffer supports partial writes. If `send()` writes only part of
the response, `outbound_offset` identifies the first byte still waiting. The
server enables `EPOLLOUT` and resumes from that offset later instead of copying
the remaining data after every write.

A per-connection outbound limit of 1 MiB protects the process from a slow
receiver or a client that intentionally causes output to accumulate. When the
limit is exceeded, the connection is closed. This is a deliberate resource
policy: protecting all active clients is more important than retaining one
unbounded queue.

### 2.7 Worker scaling with `SO_REUSEPORT`

The server can start multiple worker threads. Each worker owns:

```text
one listening socket
one EpollLoop
one ConnectionManager
```

Each listener uses `SO_REUSEPORT`, allowing multiple sockets in the same
process to bind the same port. Linux distributes new connections among those
listeners. Once accepted, a client remains with the worker that accepted it.

This ownership model was chosen to avoid transferring connection state between
threads. The normal connection path is therefore thread-confined: the worker
can access its connection map and buffers without a lock for every read or
write. More workers can use more CPU cores while preserving simple local state.

The worker count is configurable. A value of `0` selects the number of hardware
threads reported by the system, with at least one worker.

### 2.8 Shutdown and failure handling

`SIGINT` sets a shared atomic shutdown flag. Workers observe that flag between
epoll waits and leave their loops. The server joins worker threads before
returning.

Hard listener errors stop the server. Client errors remove the descriptor from
epoll, erase its connection state, and close the socket. A peer half-close is
tracked so already queued output can be flushed before the connection closes.

These choices make shutdown predictable and keep cleanup in one place instead
of depending on the operating system to reclaim resources after an abrupt exit.

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
    -> route only if status is Complete
    -> serialize the response
    -> queue it for nonblocking output
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

### 4.4 Routing

`Router` maps an exact `(method, path)` pair to:

```cpp
std::function<HttpResponse(const HttpRequest&)>
```

A matching handler receives the parsed request and returns a response. An
unknown route returns `404 Not Found`.

Exact matching was chosen for the first version because it is predictable and
small. Prefix routes, parameters, middleware, authentication, and method
fallbacks can be added later without changing the parser or socket layer.

The executable currently registers `GET /health` as a basic end-to-end check.
The telemetry contract reserves `POST /publish` for telemetry JSON, but a
publish handler and JSON validation/storage are application-level work still to
be added.

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
- Configurable port, backlog, and worker count.
- IPv4 TCP listener setup and cleanup.
- Centralized timestamped logging.
- Nonblocking sockets and `EAGAIN` handling.
- Linux epoll event loop.
- Per-worker connection management.
- `SO_REUSEPORT` worker scaling.
- Partial-write handling and bounded outbound buffering.
- Graceful signal shutdown and client cleanup.
- Docker build and concurrent TCP connection-ramp testing.
- Incremental HTTP request parsing.
- HTTP response serialization.
- Exact method/path routing.
- Parser unit tests and live health/404 checks.
- Telemetry event documentation.

Still to implement:

- Register the `POST /publish` handler.
- Validate telemetry JSON and its field ranges.
- Persist or forward accepted telemetry.
- Add authentication, rate limiting, and spam/DDoS protection.
- Add keep-alive or pipelining if required.
- Add chunked transfer encoding if required.
- Add application-level telemetry and failure tests.
