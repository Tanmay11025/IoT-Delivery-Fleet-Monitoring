# Results and Conclusions

## 1. Build and Unit-Test Results

The stale CMake cache was removed and the project was configured from the
current workspace path. The production gateway and HTTP parser test target
build successfully with C++17 and the configured compiler warnings.

The one-line command used for the local build and tests was:

```bash
cmake -S . -B build && cmake --build build --parallel && ctest --test-dir build --output-on-failure
```

Result:

```text
100% tests passed, 0 tests failed
```

The parser tests verify fragmented headers, fragmented bodies, body completion
based on `Content-Length`, requests without bodies, case-insensitive header
lookup, malformed request rejection, conflicting content lengths, unsupported
chunked encoding, and header/body size limits.

## 2. HTTP Verification Results

The running gateway was also checked over a real TCP connection:

| Request | Result |
| --- | --- |
| `GET /health` | `HTTP/1.1 200 OK` with body `{"status":"ok"}` |
| `GET /missing` | `HTTP/1.1 404 Not Found` |

Responses included `Content-Length` and `Connection: close`. These checks show
that the complete path works for a simple request: socket read, incremental
parser, router lookup, response serialization, and nonblocking response write.

## 3. HTTP Integration Test Results

The HTTP feature was tested through the real compiled gateway rather than only
by calling `HTTPParser` directly. The test uses Python's standard-library
`socket` module, not `requests`, so it controls the TCP writes and can deliver
an HTTP request one byte at a time.

### Tests used

The suite in `tests/http_integration_test.py` contains ten checks:

1. A normal `GET /health` request and response body.
2. A byte-by-byte `GET /health` request with a small delay between writes.
3. A fragmented request body with `Content-Length`.
4. Malformed request lines, HTTP versions, headers, and transfer encoding.
5. Header-name normalization and repeated headers.
6. Conflicting `Content-Length` headers.
7. Header and body size-limit rejection.
8. Exact routing for valid and unknown method/path pairs.
9. One hundred concurrent raw-socket HTTP clients.
10. A slow reader consuming the 512 KiB `/large-response` body in small chunks.

### How the test was run

Docker Compose built the gateway in one container and ran the Python test in a
separate `python:3.12-alpine` container. The test container connected to the
gateway using the Compose service name `gateway`.

The command was:

```bash
./scripts/run_http_integration_test.sh
```

The wrapper uses:

```bash
docker compose -f docker/docker-compose.http-test.yml up \
  --build \
  --abort-on-container-exit \
  --exit-code-from http-test \
  --remove-orphans
```

For the fragmented-request cases, the Python client sent individual bytes or
small request pieces and paused briefly between writes. The server therefore
had to preserve parser state across multiple readable events and wait until a
complete request was available.

For the partial-response case, the client requested `GET /large-response`.
That test-only endpoint returns 512 KiB of deterministic `x` bytes. The client
used a small receive buffer, read in 1 KiB chunks, paused between reads, and
verified the complete body and `Content-Length`. This exercised the server's
outbound buffering and `EPOLLOUT` resume path without assuming how the kernel
would split individual `send()` calls.

### Observed results

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
PASS all HTTP integration tests (10)
http-test exited with code 0
```

Docker later stopped the gateway as part of Compose teardown. Its exit code
`137` is expected in this setup because `--abort-on-container-exit` stops the
gateway after the test container finishes. The meaningful test result is the
HTTP test container's exit code `0`.

### What the results signify

The results demonstrate that the current HTTP transport works through the real
TCP server for normal, fragmented, malformed, concurrent, and backpressured
traffic. In particular, they show that:

- The parser assembles requests delivered in tiny fragments.
- The parser does not route before headers and the declared body are complete.
- Invalid framing and unsupported protocol features are rejected.
- Header names and repeated headers follow the implemented rules.
- Header and body limits are enforced.
- Exact routing returns the expected success or `404` response.
- Multiple clients can use the server concurrently without response mixing.
- A slow reader can still receive a complete large response.
- The response queue and `EPOLLOUT` path preserve the complete response body.

This is stronger evidence than the C++ parser tests alone because it includes
the real socket, epoll loop, connection state, router, response serializer, and
nonblocking write path.

The suite does not yet validate telemetry JSON or `POST /publish`, because that
application handler has not been implemented. The 50,000-client test below is
also separate: it validates the historical raw TCP echo path and connection
retention, not HTTP request processing.

## 4. Connection-Ramp Test

The Docker Compose load test uses `scripts/tcp_load_generator.py`. For each
client, it:

1. Opens a TCP connection to the gateway.
2. Sends a known payload.
3. Verifies that the payload is echoed byte-for-byte.
4. Keeps the verified connection open for the requested duration.
5. Reports connection, echo, timeout, and unexpected-close failures.

This is a TCP connection and echo stability test. It does not send HTTP
requests and therefore does not validate telemetry JSON, HTTP routing, or the
future `POST /publish` handler.

### Command used

The one-line command used for the final 50,000-connection run was:

```bash
CONNECTIONS=50000 DURATION=120 ./scripts/run_load_test.sh
```

The same command pattern was used for the other levels by changing
`CONNECTIONS` and, when needed, `DURATION`, for example:

```bash
CONNECTIONS=1000 DURATION=30 ./scripts/run_load_test.sh
```

The wrapper starts the gateway and load-test containers with Docker Compose,
rebuilds the gateway image, stops the services after the load test exits, and
returns the load generator's exit code.

The test environment used a `200000` soft and hard file-descriptor limit for
the gateway and load generator. The load-test container also used the expanded
source-port range:

```text
1024 65535
```

That setting was necessary because a single load-generator container needs a
different local ephemeral source port for each simultaneous connection to the
gateway.

### Detailed validation procedure

The validation was designed as a connection-ramp test rather than a short
port-open check. The goal was to prove that connections could remain active
and usable while the gateway continued servicing many other clients.

#### 1. Prepare the gateway

Docker Compose built the gateway image from the current C++ source and started
the server with:

```text
./build/message_gateway 8080 65535 0
```

This configuration means:

- The gateway listened on TCP port `8080`.
- The kernel listen backlog was set to `65535`.
- Worker count `0` selected one worker per detected hardware thread.
- Each worker created its own listener using `SO_REUSEPORT`.
- The gateway container allowed up to `200000` open file descriptors.

The file-descriptor limit mattered because every listening socket, epoll
descriptor, and client connection consumes an operating-system file
descriptor. Without enough descriptors, the test could fail because of the
test environment instead of the gateway's connection-handling logic.

#### 2. Prepare the load generator

The load generator ran in a separate `python:3.12-alpine` container. It also
received a `200000` file-descriptor limit and used this local ephemeral source
port range:

port, destination address, and destination port. Since all clients came from
one load-generator container and connected to the same gateway endpoint, each
client needed a different local source port.

The expanded range prevented the client side from becoming the bottleneck.

#### 3. Create the connections

The final run used:

```bash
CONNECTIONS=50000 DURATION=120 ./scripts/run_load_test.sh
```

The Python generator created clients sequentially. For each client it:

1. Created an IPv4 TCP socket.
2. Applied a connection timeout.
3. Connected to `gateway:8080`.
4. Sent the known payload:

	 ```text
	 telemetry-gateway-load-check\n
	 ```

5. Changed the socket to non-blocking mode.
6. Kept the socket in a list for later verification.

Connection or send errors were counted as failures, while the generator
continued attempting the remaining clients. This made the final report show
how many clients succeeded rather than stopping at the first error.

#### 4. Verify the response from every client

After opening the clients, the generator used Python's `selectors` module to
monitor all successful sockets. It did not create one Python thread per
client. Instead, one selector waited for sockets that had response data ready.

For each ready socket, the generator read until it had received the complete
known payload. It counted a client as failed when:

- The connection could not be created.
- The payload could not be sent.
- The server closed the socket unexpectedly.
- The response differed from the sent payload.
- The complete response did not arrive within the timeout.

This verified more than the TCP handshake: the server had to accept the
connection, receive data, process it, queue a response, send the response, and
keep the connection state valid while thousands of other clients were active.

#### 5. Hold the verified connections open

After all echo responses were verified, the generator kept the successful
clients open for `120` seconds. During this hold period it monitored the
sockets for unexpected data or peer closes.

The hold period tested connection retention. A server could accept many
For each client, the gateway followed this general path:

```text
accept()
	-> set client socket non-blocking
	-> register client with epoll
	-> read available payload bytes
	-> append response to the connection buffer
	-> send immediately or wait for EPOLLOUT
	-> keep the connection registered during the hold period
	-> remove, erase, and close after client shutdown
```

The test therefore exercised:

- TCP accept handling.
- `SO_REUSEPORT` worker distribution.
- Per-worker connection maps.
- Non-blocking `read()` behavior.
- Level-triggered `epoll` processing.
- Non-blocking `send()` behavior.
- Partial-write buffering and `EPOLLOUT`.
- Connection retention and cleanup.

#### 7. Diagnose and correct the first failed run

The first attempt targeted 50,000 clients but produced:

```text
28,230 successful
21,770 failures
```
descriptor exhaustion would not interfere with the intended test.

#### 8. Interpret the successful run

The corrected run produced:

```text
50,000 successful
0 failures
153.99 seconds total
```

The total duration included container startup, client creation, response
verification, the 120-second hold period, and teardown. It is not a pure
connection-establishment time and should not be used as an HTTP latency
measurement.

The result demonstrates that the tested gateway configuration accepted,
serviced, retained, and cleaned up 50,000 simultaneous TCP clients when both
the server and the load generator were configured with sufficient resources.

## 5. Recorded Connection Results

The project overview records the following results. The 5,000-connection run
was also part of the test sequence; its individual elapsed time was not
recorded in the overview.

| Target connections | Requested hold time | Result | Elapsed time |
| ---: | ---: | --- | ---: |
| 1,000 | 30 seconds | 1,000 successful, 0 failures | 30.14 seconds |
| 5,000 | Not recorded | Successful, 0 failures | Not recorded |
| 10,000 | 60 seconds | 10,000 successful, 0 failures | 60.97 seconds |
| 25,000 | 90 seconds | 25,000 successful, 0 failures | 98.97 seconds |
| 50,000, first attempt | 120 seconds | 28,230 successful, 21,770 failures | 39.24 seconds |
| 50,000, corrected run | 120 seconds | 50,000 successful, 0 failures | 153.99 seconds |

The final successful run means that 50,000 clients were created, their echo
payloads were verified, and the successful clients stayed connected for the
two-minute hold period without load-generator-reported failures.

The elapsed time includes container startup, connection creation, echo
verification, the hold period, and teardown. It should not be interpreted as
pure connection setup time or as a per-request latency measurement.

## 6. Conclusions

### 5.1 What the results demonstrate

The successful ramp from 1,000 through 50,000 connections demonstrates that
the current network foundation can:

- Build and start reproducibly in a Linux container.
- Accept large numbers of simultaneous IPv4 TCP connections.
- Distribute connections across `SO_REUSEPORT` workers.
- Process readable sockets through the nonblocking `epoll` event loop.
- Echo data correctly for every verified client.
- Keep tens of thousands of connections open during a sustained hold period.
- Handle client cleanup after the load generator closes connections.

The zero-failure results at 10,000, 25,000, and the corrected 50,000 run give
useful evidence that the worker ownership model, per-connection state,
epoll-driven reads, and outbound buffering work together under a large
connection count.

### 5.2 What the first 50,000 failure taught us

The first 50,000-client attempt stopped at about 28,230 successful connections
with 21,770 failures. This closely matches the usual Linux ephemeral source-port
This is an important operational conclusion: high-connection tests must
configure both sides of the test. Increasing the gateway's file-descriptor
limit alone would not have fixed the first run because the client generator
could not create enough unique local TCP endpoints.

### 5.3 What the results do not demonstrate

These tests do not yet prove:

- HTTP throughput or HTTP latency.
- Correct telemetry JSON validation.
- A registered or functioning `POST /publish` application handler.
- Authentication, authorization, rate limiting, or DDoS protection.
- Durable telemetry storage or downstream broadcasting.
- Behavior under slow readers and deliberate outbound queue saturation.
- Recovery after a process crash or host failure.
- Multi-host load generation or production network performance.

The current results therefore validate the transport foundation and its
connection scalability, not the complete fleet-monitoring product.

## 7. Overall Result

The project has moved from a basic TCP server to a scalable, event-driven
transport with an incremental HTTP layer. The load results show that the
networking foundation is capable of maintaining 50,000 simultaneous tested
