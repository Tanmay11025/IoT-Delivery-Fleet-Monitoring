#pragma once

#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <csignal>
#include <string>
#include <cerrno>
#include <sys/epoll.h>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include "../core/logger.h"
#include "../http/router.h"
#include "connection_manager.h"
#include "epoll_loop.h"

using namespace std;

// All workers read this flag between epoll waits.
extern atomic<bool> shutdown_requested;

// Set the shutdown flag when the user presses Ctrl+C.
void handle_sigint(int);

// Configure a file descriptor so I/O returns immediately when it would block.
bool set_nonblocking(int fd);

// Create a nonblocking IPv4 listener that can be shared by reuse-port workers.
int create_reuseport_listener(int port, int backlog);

class TCPServer {
public:
    // Store the configuration used to create and listen on the server socket.
    TCPServer(int port, int backlog, unsigned int worker_count = 0);

    // Release the server socket when the object leaves scope.
    ~TCPServer();

    // A server owns its socket and cannot be copied safely.
    TCPServer(const TCPServer&) = delete;
    TCPServer& operator = (const TCPServer&) = delete;

    // Create the socket, begin listening, and accept clients until stopped.
    bool start();

    // Request all worker loops to stop. Calling this more than once is safe.
    void stop();

    // Accessors for server configuration (read-only)
    int get_port() const { return port; }
    int get_backlog() const { return backlog; }
    unsigned int get_worker_count() const { return worker_count; }

    size_t active_connection_count() const {
        return active_connections.load(memory_order_relaxed);
    }

    void add_route(string method, string path, Router::Handler handler) {
        router.add_route(std::move(method), std::move(path), std::move(handler));
    }

private:
    // Run one complete listener, epoll loop, and connection manager.
    void worker_loop(unsigned int worker_id);

    // Decide what to do when one descriptor becomes ready.
    void handle_event(EpollLoop& loop, ConnectionManager& connections,
                      int listener_fd, int fd, unsigned int events);

    // Accept every queued connection without blocking the event loop.
    void accept_clients(EpollLoop& loop, ConnectionManager& connections, int listener_fd);

    // Read all available client data, parse one request, and queue its response.
    void handle_client(EpollLoop& loop, ConnectionManager& connections, int client_fd);

    // Send as much buffered data as the socket currently accepts.
    bool flush_client(EpollLoop& loop, ConnectionManager& connections, int client_fd);

    // Remove a client from epoll and release its socket.
    void close_client(EpollLoop& loop, ConnectionManager& connections, int client_fd);

    // A slow receiver may otherwise grow its queued echo data without bound.
    static constexpr size_t max_outbound_bytes = 1U << 20; // 1 MiB per client

    int port;
    int backlog;
    unsigned int worker_count;
    atomic<bool> worker_failed{false};
    atomic<size_t> active_connections{0};
    vector<thread> workers;
    Router router;
};
