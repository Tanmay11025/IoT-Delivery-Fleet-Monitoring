# Telemetry Gateway: Project Overview

## 1. What is this project?

Telemetry Gateway is a high-performance network service for receiving telemetry data from many clients.

The long-term goal is to:

- Handle large traffic spikes.
- Reduce spam and DDoS traffic.
- Buffer valid incoming requests.
- Broadcast telemetry to many downstream systems in real time.

The current implementation is the network foundation for those features. It accepts TCP clients and processes their data without blocking the whole server.

## 2. Current implementation

The program currently follows this flow:

1. `main.cpp` reads the port and connection backlog from the command line.
2. It validates those values and creates a `TCPServer`.
3. `TCPServer` creates and binds an IPv4 TCP socket.
4. The server starts listening for incoming connections.
5. The listening socket and each client socket are set to non-blocking mode.
6. `EpollLoop` waits for sockets that are ready for work.
7. `TCPServer` accepts new clients, reads available data, and echoes it back.
8. `ConnectionManager` keeps track of active client connections.
9. Disconnected clients are removed from `epoll`, removed from the manager, and closed.

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

## 5. Why separate the code into components?

Each component has one main responsibility:

- `main.cpp`: validates startup arguments and starts the server.
- `TCPServer`: owns server behavior, socket setup, accepting clients, and client handling.
- `EpollLoop`: owns the operating-system event loop.
- `ConnectionManager`: stores the active client connections.
- `logger.h`: provides centralized logging.

This separation makes the code easier to understand, test, and extend. For example, buffering or message processing can be added without putting all of that logic inside the `epoll` implementation.

## 6. Why use RAII and explicit cleanup?

A socket is an operating-system resource. It must be closed when it is no longer needed.

`TCPServer` closes its listening socket in its destructor. `EpollLoop` closes its `epoll` descriptor in its destructor. Client sockets are removed from `epoll` and closed when clients disconnect or an error occurs.

This prevents resource leaks and makes shutdown more predictable.

## 7. Why is this implementation important?

This implementation establishes the gateway's basic traffic-handling layer. It provides:

- TCP connectivity.
- Non-blocking I/O.
- Event-driven processing.
- Active connection tracking.
- Basic error handling and cleanup.

These capabilities are required before adding traffic filtering, buffering, telemetry parsing, and broadcasting. Without this foundation, later features would struggle to handle many simultaneous clients reliably.

## 8. What is implemented and what is planned?

### Implemented

- Configurable TCP port and connection backlog.
- IPv4 socket setup and listening.
- Non-blocking server and client sockets.
- `epoll`-based event loop.
- Client connection tracking.
- Basic data echoing.
- SIGINT-based shutdown request.
- Resource cleanup.

### Planned next

- Parse and validate telemetry messages.
- Add a high-speed buffer or queue.
- Add spam and DDoS protection.
- Add backpressure when downstream systems are slow.
- Broadcast valid telemetry to downstream consumers.
- Add metrics, load tests, and failure tests.

The current echo behavior is a simple connectivity test. It will eventually be replaced or extended by the telemetry-processing pipeline.

## 9. Current limitations

The project is still an early networking foundation. It does not yet provide:

- Telemetry message parsing.
- Authentication or authorization.
- Rate limiting or DDoS mitigation.
- Persistent buffering.
- Downstream broadcasting.
- Production-level observability.

These limitations are intentional at this stage: reliable connection handling is being established before higher-level gateway behavior is added.

## 10. Keeping this document updated

This document is maintained alongside the project. Whenever implementation changes are synced to GitHub, update this file when the change affects:

- The project architecture.
- A design decision or its reasoning.
- The list of implemented features.
- The planned work or current limitations.

Each update should briefly explain what changed, why it was chosen, and how it supports the gateway's goals. Small internal fixes that do not change the design or project direction do not need a documentation update.
