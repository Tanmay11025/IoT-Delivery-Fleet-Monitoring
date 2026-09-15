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
| `GET /health` | `HTTP/1.1 200 OK` with body `OK` |
| `GET /missing` | `HTTP/1.1 404 Not Found` |

Responses included `Content-Length` and `Connection: close`. These checks show
that the complete path works for a simple request: socket read, incremental
parser, router lookup, response serialization, and nonblocking response write.

## 3. Connection-Ramp Test

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

## 4. Recorded Connection Results

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

## 5. Conclusions

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
range of approximately 28,232 ports (`32768` through `60999`).

The limiting resource was the load generator's local source-port pool, not a
proven gateway connection limit. After changing the load-test container's
range to `1024 65535`, the same target completed with 50,000 successful
connections and zero failures.

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

## 6. Overall Result

The project has moved from a basic TCP server to a scalable, event-driven
transport with an incremental HTTP layer. The load results show that the
networking foundation is capable of maintaining 50,000 simultaneous tested
connections when the test environment is configured correctly. The next
meaningful result should come from an HTTP-aware load test that sends telemetry
JSON to `POST /publish` and measures parsing, validation, routing, and
application processing rather than only echo behavior.
