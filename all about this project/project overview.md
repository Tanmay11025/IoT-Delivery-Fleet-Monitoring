# Telemetry Gateway: Project Overview

## Quick orientation

This repository is building a Linux TCP gateway for telemetry traffic. The current milestone is a small, event-driven TCP server that can keep multiple client connections open, read available data, and echo that data back.

The echo response is deliberately simple: every byte received is sent back to
the same client. It proves that connection setup, event notification,
non-blocking I/O, data transfer, backpressure, and cleanup work before
telemetry-specific processing is added.

This document describes the code that exists today. It does not claim that the
gateway already implements telemetry parsing or authentication. It also records
the Docker-based connection-ramp results completed so far.

At a high level, each worker owns one listener, event loop, and connection
manager. Linux `SO_REUSEPORT` distributes new connections across those workers;
an accepted connection remains in the accepting worker for its entire lifetime.

```text
Worker 0: listener -> EpollLoop -> ConnectionManager -> clients
Worker 1: listener -> EpollLoop -> ConnectionManager -> clients
Worker N: listener -> EpollLoop -> ConnectionManager -> clients
```

This ownership rule is important. A worker never passes an accepted client to
another worker. As a result, its connection map and outbound buffers are only
used by the one thread that owns them; the normal data path needs no locks.

## Run the current server

From the repository root, build and start the server with:

```bash
cmake -S . -B build
cmake --build build
./build/message_gateway [port] [backlog] [workers]
```

The defaults are port `8080`, backlog `128`, and one worker per detected core. For example:

```bash
./build/message_gateway 9000 256
```

The server can be stopped with `Ctrl+C`. It is an echo server at this stage:
bytes sent by a client are returned to that same client.

## 1. What is this project?

Telemetry Gateway is a high-performance network service for receiving telemetry data from many clients.

The long-term goal is to:

- Handle large traffic spikes.
- Buffer valid incoming requests.
- Broadcast telemetry to many downstream systems in real time.

The current implementation is the network foundation for those features. It accepts TCP clients and processes their data without blocking the whole server.

## 2. Current implementation, from startup to shutdown

The program currently follows this flow:

1. `main.cpp` reads the port, connection backlog, and worker count from the command line.
2. It validates those values and creates a `TCPServer`.
3. Each worker creates and binds an IPv4 TCP socket with `SO_REUSEPORT`.
4. Each worker starts listening and registers its listener with its own `EpollLoop`.
5. The listening socket and each client socket are set to non-blocking mode.
6. Each worker accepts only the clients selected for its listener.
7. The worker reads available data and appends echo responses to that client's
   per-connection outbound buffer.
8. The worker sends as much of that buffer as the operating system accepts.
   If `send()` reports `EAGAIN`, `EPOLLOUT` is enabled and the unsent bytes
   remain queued until the socket becomes writable again.
9. `ConnectionManager` keeps each worker's active client state private to that worker.
10. A client with more than 1 MiB waiting in its outbound queue is disconnected.
    This protects the process from a slow or malicious receiver consuming all
    available memory.
11. Disconnected clients are removed from `epoll`, removed from the manager,
    and closed. `Ctrl+C` sets a shared atomic shutdown flag; each event loop
    checks it between waits and exits cleanly.

### Worker count and `SO_REUSEPORT`

The third command-line argument is the worker count. If it is omitted, or is
`0`, the server uses one worker for every hardware thread reported by the
machine, with a minimum of one worker.

Each worker calls `create_reuseport_listener(port, backlog)`. That helper:

1. creates an IPv4 TCP socket;
2. enables `SO_REUSEADDR` for convenient restarts and `SO_REUSEPORT` so all
   workers may bind the same address and port;
3. binds and listens on the configured port and backlog;
4. sets the listener to non-blocking mode; and
5. closes the socket and returns failure on every setup error.

On Linux, `SO_REUSEPORT` lets the kernel select a listening worker for each new
connection. It is not a helper around one central listener: every worker has
its own listener, epoll instance, and connection manager.

## How one client request is handled

1. The listening socket is registered with `epoll` for `EPOLLIN`, meaning that a connection may be waiting.
2. When `epoll_wait()` reports the listening socket, the server calls `accept()` repeatedly until the non-blocking socket reports `EAGAIN` or `EWOULDBLOCK`.
3. Each new client descriptor is set to non-blocking mode and registered for readable data and peer half-close events.
4. The descriptor is added to `ConnectionManager` so the server can track it as active.
5. When client data arrives, `epoll` reports `EPOLLIN` and the server drains all currently available bytes with `read()`.
6. The server appends each read chunk to the connection's outbound buffer and
   sends as much as the socket accepts.
7. If `send()` returns `EAGAIN`, `EPOLLOUT` is registered and the remaining
   bytes stay buffered until the socket is writable. `EAGAIN` is normal for a
   non-blocking socket with a full kernel send buffer; it is not a failure.
8. The outbound buffer stores an offset to the first unsent byte. This avoids
   repeatedly copying the whole remaining buffer after each partial send.
9. When a client half-closes its write side (`EPOLLRDHUP`), the server still
   flushes bytes already queued for that client before closing its side.
10. A hard socket error, full close, or outbound queue above 1 MiB causes the
    client to be unregistered and closed.

The event loop is allowed to wait inside `epoll_wait()` when no socket needs attention. This is different from repeatedly polling every client in a busy loop.

## 3. Why use non-blocking sockets?

A blocking socket waits until an operation can complete. For example, a read could wait indefinitely for a client to send data.

That would allow one slow client to delay the entire server.

Non-blocking sockets return immediately when no work is available. The server can then continue handling other clients. The code checks for `EAGAIN` or `EWOULDBLOCK`, which means that the socket is currently out of work rather than broken.

## 4. Why use `epoll`?

`epoll` is a Linux event-notification mechanism. It watches many file descriptors and reports only the ones that are ready.

This design is better suited to a gateway than creating one blocking loop per client because:

- One event loop can manage many connections.
- The server does not repeatedly wait on inactive clients.
- CPU time is spent processing sockets that have actual activity.
- The design can be extended for high connection counts.

The loop currently uses level-triggered events. It keeps reporting a socket while it still has readable data.

Client sockets also watch for `EPOLLRDHUP`, which detects a peer half-close.
The server handles readable data first and flushes a queued echo response
before closing. This preserves data that arrived just before the peer stopped
writing.

## 5. Backpressure and memory safety

A fast sender and a slow receiver are a normal situation on a busy network.
The server must not block one worker waiting for that receiver, and it must not
allow the receiver to create an unlimited in-memory queue.

For each connection, `Connection` stores:

- the client file descriptor;
- an outbound string containing bytes waiting to be sent;
- an offset identifying the first unsent byte; and
- a flag recording whether the peer has stopped writing.

When the kernel cannot accept more output, the server leaves the unsent bytes
in this queue and asks epoll to notify it with `EPOLLOUT`. The queue is capped
at 1 MiB per client. Exceeding the cap closes that client, which is a deliberate
resource-protection policy. Once a large queue drains, its large allocation is
released instead of being retained for the rest of the connection.

## 6. Why separate the code into components?

Each component has one main responsibility:

- `main.cpp`: validates startup arguments and starts the server.
- `TCPServer`: owns worker lifecycle, reuse-port socket setup, accepting clients, and client handling.
- `EpollLoop`: owns the operating-system event loop.
- `ConnectionManager`: stores the active client connections.
- `logger.h`: provides centralized logging.

The main source files are organized as follows:

```text
src/
├── main.cpp                         Startup arguments and server launch
├── core/logger.h                    Shared logging helper
└── network/
	├── tcp_server.h/.cpp            Socket setup and client behavior
	├── epoll_loop.h/.cpp            Linux event monitoring
	└── connection_manager.h/.cpp    Active client tracking
```

`CMakeLists.txt` builds these files into the `message_gateway` executable. The project currently targets C++17 and enables `-Wall`, `-Wextra`, and `-Wpedantic` compiler warnings.

This separation makes the code easier to understand, test, and extend. For example, buffering or message processing can be added without putting all of that logic inside the `epoll` implementation.

## 7. Cleanup, logging, and shutdown

A socket is an operating-system resource. It must be closed when it is no longer needed.

Each worker closes its own listening socket when its loop ends. `EpollLoop`
closes its `epoll` descriptor in its destructor. Client sockets are removed
from `epoll` and closed when clients disconnect or an error occurs. `TCPServer`
requests shutdown and joins worker threads in its destructor.

The logger protects its output with a mutex. Startup messages from several
workers therefore do not interleave. The server intentionally does not log
every client connection, because per-connection logging can dominate CPU and
disk I/O during high connection rates.

This prevents resource leaks and makes shutdown more predictable.

## 8. Why is this implementation important?

This implementation establishes the gateway's basic traffic-handling layer. It provides:

- TCP connectivity.
- Non-blocking I/O.
- Event-driven processing.
- Active connection tracking.
- Basic error handling and cleanup.

These capabilities are required before adding traffic filtering, buffering, telemetry parsing, and broadcasting. Without this foundation, later features would struggle to handle many simultaneous clients reliably.

## 9. What is implemented and what is planned?

### Implemented

- Configurable TCP port, connection backlog, and worker count.
- One reuse-port listener, event loop, and connection manager per worker.
- IPv4 socket setup and listening.
- Non-blocking server and client sockets.
- `epoll`-based event loop.
- Client connection tracking.
- Basic data echoing with per-connection outbound buffering and `EPOLLOUT` backpressure handling.
- A 1 MiB per-client outbound-buffer limit and efficient partial-write tracking.
- SIGINT-based shutdown request.
- Thread-safe startup logging and atomic worker shutdown coordination.
- Graceful handling of client half-closes and listening-socket failures.
- Resource cleanup.

### Planned next

- Parse and validate telemetry messages.
- Add a high-speed buffer or queue.
- Add spam and DDoS protection.
- Add backpressure when downstream systems are slow.
- Broadcast valid telemetry to downstream consumers.
- Add metrics and additional Docker-based failure tests, especially slow-reader
  and queue-limit behavior.

The current echo behavior is a simple connectivity test. It will eventually be replaced or extended by the telemetry-processing pipeline.

## 10. Current limitations

The project is still an early networking foundation. It does not yet provide:

- Telemetry message parsing.
- Authentication or authorization.
- Rate limiting or DDoS mitigation.
- Persistent buffering.
- Downstream broadcasting.
- Production-level observability.
- A configurable queue-limit policy; the current 1 MiB limit is a safe default,
  not a workload-specific tuning decision.

These limitations are intentional at this stage: reliable connection handling is being established before higher-level gateway behavior is added.

## 11. The socket lifecycle in simple terms

The server follows the standard TCP lifecycle:

1. `socket()` asks the operating system for a network endpoint and returns a file descriptor.
2. `bind()` assigns the server's IP address and port. This server uses `INADDR_ANY`, so it listens on all local interfaces.
3. `listen()` enables incoming connections and creates a kernel-managed backlog queue.
4. `accept()` removes a waiting connection from that queue and returns a new file descriptor for that client.
5. `read()` receives client bytes and `send()` returns the bytes in the current echo test.
6. `close()` releases the client or server descriptor.

A file descriptor is the small integer used by the program to refer to a kernel resource such as a socket. The listening descriptor and each client descriptor are different: the first accepts connections, while the others carry client data.

TCP was chosen because telemetry delivery needs an ordered and reliable byte stream. UDP may be faster in some cases, but it does not guarantee delivery or ordering.

The port is converted with `htons()` because network protocols use a standard byte order. This ensures the port is interpreted correctly by the operating system regardless of the machine's native byte order.

## 12. Deployment and Docker-based testing preparation

`epoll` reports events using flags such as `EPOLLIN` for readable data and
`EPOLLRDHUP` when a peer closes its writing side. These flags let the server
decide what action is needed for each descriptor.

The operating system limits how many file descriptors a process may have open.
A production deployment must configure `RLIMIT_NOFILE` or the container
`nofile` limit. The load-generator container also needs enough ephemeral ports,
file descriptors, and socket memory. `net.core.somaxconn` and the TCP SYN
backlog cap the effective listen backlog.

The testing described below is a Docker Compose connection-ramp test. It tests
the current TCP echo behavior under many simultaneous connections. It is not a
telemetry-format, HTTP, authentication, or end-to-end fleet-monitoring test.

### 12.1 Docker test topology

The Compose file defines two containers:

```text
                         Docker Compose network

  loadtest container                         gateway container
  python:3.12-alpine                         Ubuntu 24.04 image
  tcp_load_generator.py  ── TCP ──────────>  message_gateway:8080
       many clients       <── echo ─────────
```

The `gateway` service is built from `docker/Dockerfile`. The Dockerfile starts
from Ubuntu 24.04, installs `build-essential` and CMake, copies the repository
into `/app`, configures the CMake project, and compiles the
`message_gateway` executable. The service then starts:

```text
./build/message_gateway 8080 65535 0
```

This means that the gateway listens on port `8080`, uses a listen backlog of
`65535`, and uses the default worker behavior. With worker count `0`, the
server creates one worker per detected hardware thread, with a minimum of one.
The gateway container has a `nofile` soft and hard limit of `200000`, giving it
enough descriptor capacity for the listening sockets, epoll descriptors, and a
large number of client sockets. Port `8080` is published to the host for
optional inspection, although the load generator reaches the gateway through
the Compose service name `gateway` on the internal Docker network.

The `loadtest` service uses the small `python:3.12-alpine` image. It mounts
`scripts/tcp_load_generator.py` read-only as `/load_test.py` and runs it with
Python. It also receives these environment variables:

```text
CONNECTIONS    number of client sockets to create
DURATION       number of seconds to keep successful clients open
LOADTEST_HOST  gateway
LOADTEST_PORT  8080
```

The load-generator container also has a `nofile` soft and hard limit of
`200000`. Its network namespace is configured with:

```yaml
net.ipv4.ip_local_port_range: "1024 65535"
```

This expands the range of local ephemeral source ports available to the
generator. A single source IP and destination pair cannot reuse the same local
source port for multiple simultaneous TCP connections, so this setting is
important when one container creates tens of thousands of clients.

The repository's Compose-based test uses the Python generator shown above. The
commands documented here do not invoke tcpkali2; a tcpkali2 run would be a
different load-generator implementation and would need its own image, command,
and result-reporting configuration.

### 12.2 What the load generator does

The test generator is deliberately dependency-free and validates the gateway's
actual TCP echo path rather than only checking whether a port is open. For each
run, it performs the following sequence.

1. It reads `CONNECTIONS`, `DURATION`, and `LOADTEST_PORT` as positive integer
   values. The host defaults to `gateway` if `LOADTEST_HOST` is not supplied.
2. It waits for Docker DNS and the gateway listener to become available. It
   repeatedly attempts a short TCP connection for up to ten seconds. This
   avoids treating normal container startup time as a load-test failure.
3. It creates the requested number of IPv4 TCP client sockets sequentially.
   Each client connects to `gateway:8080` and sends the known payload:

   ```text
   telemetry-gateway-load-check\n
   ```

4. After sending, each socket is changed to non-blocking mode and retained in
   memory. The generator records connection or send errors as failures but
   continues creating the remaining clients.
5. It registers all successfully opened sockets with Python's selector API.
   The selector waits until individual sockets have data available, allowing
   one generator process to verify many sockets without one blocking thread per
   connection.
6. It reads from each ready socket until the complete payload has been
   received. The response must match the payload byte-for-byte. A closed
   socket, a mismatched response, or a socket that does not complete within
   thirty seconds counts as a failure.
7. If echo verification succeeds, it keeps the verified sockets open for the
   requested `DURATION`. This is the concurrent-connection portion of the
   test: the gateway must maintain all those established connections while the
   generator waits.
8. During the hold period, unexpected data or a peer close is counted as a
   failure. At the end, the generator closes its client sockets and prints a
   summary containing the requested connections, successful connections,
   failures, and elapsed time. It exits with status `0` only when there are no
   reported failures.

This sequence checks more than the TCP handshake. It checks connection
creation, data arrival, echo response, non-blocking event handling, connection
retention, and cleanup from the client side.

### 12.3 Command used to verify the source-port range

Before the main run, the following command was used:

```bash
docker compose -f docker/docker-compose.loadtest.yml run --rm --no-deps loadtest \
  cat /proc/sys/net/ipv4/ip_local_port_range
```

The command creates a temporary `loadtest` container, does not start the
`gateway` dependency because of `--no-deps`, runs `cat` instead of the normal
Python command, prints the kernel setting from inside that container, and
removes the temporary container after completion because of `--rm`.

The expected output is:

```text
1024 65535
```

Checking the value inside the container matters because network namespaces can
have settings different from the host. It confirms that the generator has a
large enough local source-port range for the 50,000-connection run.

### 12.4 Command used to run the connection test

The main test command was run from the repository root:

```bash
CONNECTIONS=50000 DURATION=120 ./scripts/run_load_test.sh
```

The wrapper script executes:

```bash
docker compose -f docker/docker-compose.loadtest.yml up \
  --build \
  --abort-on-container-exit \
  --exit-code-from loadtest \
  --remove-orphans
```

The options have these effects:

- `-f docker/docker-compose.loadtest.yml` selects the load-test Compose file.
- `up` creates the gateway and loadtest services and starts them together.
- `--build` rebuilds the gateway image when required, so the test uses the
  current source and does not depend on an old image.
- `--abort-on-container-exit` stops the other service when one service exits.
  Therefore, once the load generator finishes, Compose intentionally stops the
  gateway container.
- `--exit-code-from loadtest` makes the overall Compose command report the
  load-generator status. A zero status means the generator reported no
  failures; a non-zero status means the generator found a load-test failure.
- `--remove-orphans` removes containers from the same Compose project that are
  no longer defined by the current file.

For this run, `CONNECTIONS=50000` overrides the Compose default of `1000`, so
the generator attempts 50,000 simultaneous clients. `DURATION=120` overrides
the default hold time of 30 seconds, so verified clients remain connected for
two minutes. The elapsed time can be greater than 120 seconds because it also
includes container startup, connection creation, echo verification, and
shutdown.

### 12.5 What happens inside the gateway during the test

When the gateway container starts, every worker creates its own IPv4 listening
socket on port `8080`. The sockets use `SO_REUSEPORT`, so Linux distributes new
connections across the workers. Each worker owns its listener, epoll instance,
and connection manager.

As the load generator opens clients:

1. A worker's listening socket becomes readable in epoll.
2. The worker calls `accept()` repeatedly until no more pending connections are
   immediately available.
3. Each accepted client socket is made non-blocking and registered with epoll.
4. When the generator sends the known payload, epoll reports the client socket
   as readable.
5. The worker reads the available bytes, appends them to that connection's
   outbound buffer, and sends the bytes back to the same client.
6. If the kernel send buffer temporarily cannot accept all output,
   `EPOLLOUT` is enabled and the remaining bytes stay in the per-connection
   outbound buffer until the socket becomes writable.
7. While the generator holds the connections open, the worker event loops keep
   the sockets registered without busy-polling inactive clients.
8. When the generator closes the sockets after the hold period, the gateway
   observes the peer close, removes the descriptors from epoll, releases the
   connection state, and closes the client sockets.

The test therefore exercises the same accept, epoll, read, echo, buffering,
partial-write, and cleanup paths used by ordinary clients. It does not exercise
telemetry parsing because the current server echoes arbitrary bytes and does
not yet parse the payload as a telemetry message.

### 12.6 Connection-ramp results and interpretation

Testing was performed as a gradual connection ramp before the final 50,000
connection run. The recorded results were:

| Target connections | Requested hold time | Result | Elapsed time |
| ---: | ---: | --- | ---: |
| 100 | 15 seconds | 100 successful, 0 failures | 15.05 seconds |
| 1,000 | 30 seconds | 1,000 successful, 0 failures | 30.14 seconds |
| 10,000 | 60 seconds | 10,000 successful, 0 failures | 60.97 seconds |
| 25,000 | 90 seconds | 25,000 successful, 0 failures | 98.97 seconds |
| 50,000, first attempt | 120 seconds | 28,230 successful, 21,770 failures | 39.24 seconds |
| 50,000, after generator-port update | 120 seconds | 50,000 successful, 0 failures | 153.99 seconds |

The final successful result means that 50,000 client connections were created,
their echo payloads were verified, and the successful connections remained
open for the requested two-minute hold period without generator-reported
failures. The 153.99-second total includes work before and after the hold
period, so it is not a claim that connection setup and verification took only
33.99 seconds in isolation.

The first 50,000-connection attempt stopped at approximately 28,230 successful
clients. This closely matched the usual Linux default ephemeral-port range of
`32768` through `60999`, which provides about 28,232 ports. The limiting
resource was the source-port pool of the single load-generator container, not
the gateway's advertised connection capacity. Each simultaneous connection
from that generator to `gateway:8080` needs a distinct local source port.

After setting the generator container's range to `1024 65535`, the available
source-port pool was large enough for the requested 50,000 clients and the
test completed successfully. The gateway and load generator were also given
large file-descriptor limits so that descriptor exhaustion would not occur
before the intended connection-capacity test.

### 12.7 Teardown and exit code interpretation

The load generator is the service whose exit status represents the test result.
When it finishes its two-minute hold and closes its clients, it exits. Because
the Compose command uses `--abort-on-container-exit`, Compose then stops the
gateway container even though the gateway itself has not failed.

As a result, the gateway may appear with exit code `137` during this test. That
code is an expected consequence of Compose stopping the gateway container as
part of test teardown; it is not evidence that the gateway failed during the
connection ramp. The meaningful result is the load generator's summary and the
exit code selected by `--exit-code-from loadtest`.

### 12.8 What this test proves and does not prove

The Docker test provides evidence that the current gateway can:

- Build and start reproducibly inside a Linux container.
- Accept a large number of simultaneous IPv4 TCP connections.
- Distribute accepted connections across reuse-port workers.
- Receive and echo data through the non-blocking epoll path.
- Maintain verified connections for a sustained hold period.
- Handle client cleanup after the load generator closes connections.
- Operate with explicitly configured descriptor and source-port limits.

The test does not prove:

- Correct telemetry parsing or validation.
- Authentication, authorization, rate limiting, or DDoS protection.
- Durable buffering or downstream broadcasting.
- Performance under realistic telemetry message rates.
- Slow-reader behavior or deliberate outbound queue saturation.
- Multi-host load generation or production deployment behavior.
- Failure recovery after a process crash or host failure.

The current echo behavior is a connectivity and concurrency test. It will
eventually be supplemented or replaced by tests that send real telemetry
messages and exercise the processing, buffering, and downstream delivery
pipeline.

## 13. Keeping this document updated

This document is maintained alongside the project. Whenever implementation changes are synced to GitHub, update this file when the change affects:

- The project architecture.
- A design decision or its reasoning.
- The list of implemented features.
- The planned work or current limitations.

Each update should briefly explain what changed, why it was chosen, and how it supports the gateway's goals. Small internal fixes that do not change the design or project direction do not need a documentation update.
