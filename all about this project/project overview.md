# Telemetry Gateway: Project Overview

## Quick orientation

This repository is building a Linux TCP gateway for telemetry traffic. The current milestone is a small, event-driven TCP server that can keep multiple client connections open, read available data, and echo that data back.

The echo response is deliberately simple. It proves that connection setup, event notification, non-blocking I/O, data transfer, and cleanup work before telemetry-specific processing is added.

At a high level:

```text
Client connections
		|
		v
TCPServer -> EpollLoop -> ready socket events
	|                              |
	+-> accept/read/send ----------+
	|
	+-> ConnectionManager tracks active clients
```

## Run the current server

From the repository root, build and start the server with:

```bash
cmake -S . -B build
cmake --build build
./build/message_gateway [port] [backlog]
```

The defaults are port `8080` and backlog `128`. For example:

```bash
./build/message_gateway 9000 256
```

The server can be stopped with `Ctrl+C`. A simple TCP client such as `nc` can be used to send test data and verify the current echo behavior.

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
9. Client data is read before disconnect events are handled, so final bytes are not discarded.
10. Disconnected clients are removed from `epoll`, removed from the manager, and closed.

## How one client request is handled

1. The listening socket is registered with `epoll` for `EPOLLIN`, meaning that a connection may be waiting.
2. When `epoll_wait()` reports the listening socket, the server calls `accept()` repeatedly until the non-blocking socket reports `EAGAIN` or `EWOULDBLOCK`.
3. Each new client descriptor is set to non-blocking mode and registered for readable data and peer half-close events.
4. The descriptor is added to `ConnectionManager` so the server can track it as active.
5. When client data arrives, `epoll` reports `EPOLLIN` and the server drains all currently available bytes with `read()`.
6. The server sends each read chunk back with `send()`. The send loop handles partial writes instead of assuming one call sends everything.
7. A return value of `0`, a hangup, or an unrecoverable error causes the server to unregister and close that client.

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

Client sockets also watch for `EPOLLRDHUP`, which detects a peer half-close. The server handles readable data first and closes the client afterward. This preserves data that arrived just before the peer disconnected.

## 5. Why separate the code into components?

Each component has one main responsibility:

- `main.cpp`: validates startup arguments and starts the server.
- `TCPServer`: owns server behavior, socket setup, accepting clients, and client handling.
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
- Graceful handling of client half-closes and listening-socket failures.
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

## 10. The socket lifecycle in simple terms

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

## 11. Scaling considerations

`epoll` reports events using flags such as `EPOLLIN` for readable data and `EPOLLRDHUP` when a peer closes its writing side. These flags let the server decide what action is needed for each descriptor.

The operating system also limits how many file descriptors a process may have open. A production deployment must configure an appropriate file-descriptor limit before accepting a large number of clients. This is an operational requirement for the planned high-traffic gateway, not yet a complete feature of the current server.

## 12. Keeping this document updated

This document is maintained alongside the project. Whenever implementation changes are synced to GitHub, update this file when the change affects:

- The project architecture.
- A design decision or its reasoning.
- The list of implemented features.
- The planned work or current limitations.

Each update should briefly explain what changed, why it was chosen, and how it supports the gateway's goals. Small internal fixes that do not change the design or project direction do not need a documentation update.
