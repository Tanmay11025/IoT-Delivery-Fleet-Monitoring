#pragma once

#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <arpa/inet.h>
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
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include "../core/logger.h"
#include "../core/config.h"
#include "../http/router.h"
#include "connection_manager.h"
#include "epoll_loop.h"
#include "../monitoring/metrics.h"
#include "thread_pool.h"
#include "../storage/mmap_log.h"
#include "../telemetry/fleet_state.h"
#include <unordered_map>

using namespace std;

// All workers read this flag between epoll waits.
extern atomic<bool> shutdown_requested;

// Set the shutdown flag when SIGINT or SIGTERM is received.
void handle_shutdown_signal(int);

// Configure a file descriptor so I/O returns immediately when it would block.
bool set_nonblocking(int fd);

// Create a nonblocking IPv4 listener that can be shared by reuse-port workers.
int create_reuseport_listener(int port, int backlog);

class TCPServer {
public:
    explicit TCPServer(Config config);

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
    int get_port() const { return config.port; }
    int get_backlog() const { return config.backlog; }
    unsigned int get_worker_count() const { return epoll_worker_count; }

    size_t active_connection_count() const {
        return metrics.active_connections();
    }

    string metrics_text() const;

    std::string recent_messages_json(std::size_t count = 100) const;
    HttpResponse recent_messages_response(const HttpRequest& request) const;

    // Append an opaque record and wake any streaming consumers.
    std::uint64_t append_message(const std::string& payload);

    // POST /publish: validate a TelemetryEvent, append it, update fleet state.
    HttpResponse publish_telemetry(const HttpRequest& request);

    // GET /vehicles and GET /consumers bodies.
    std::string vehicles_json() const { return fleet_state.to_json(); }
    std::string consumers_json() const;

    void add_route(string method, string path, Router::Handler handler) {
        router.add_route(std::move(method), std::move(path), std::move(handler));
    }

private:
    struct Completion {
        int client_fd;
        std::uint64_t generation;
        string serialized_response;
        int status_code;
        chrono::duration<double> elapsed;
    };

    struct LoopState {
        int completion_fd = -1;
        int consumer_listener_fd = -1;
        mutex completion_mutex;
        queue<Completion> completions;
        atomic<size_t> outstanding_tasks{0};
        // Set by publishers (pool threads) when new records exist, so the
        // worker pushes them to its consumers on the next wake-up.
        atomic<bool> data_available{false};
        // Mirrors ConnectionManager::consumer_count() for publisher threads,
        // which skip the eventfd write when a worker has no consumers.
        atomic<size_t> consumer_count{0};
        std::uint64_t next_generation = 1;
        std::size_t worker_id = 0;
        bool draining = false;
    };

    // What /consumers and the lag metric report about one consumer.
    struct ConsumerInfo {
        std::string client_ip;
        bool streaming = false;
        std::uint64_t cursor = 0;
        std::uint64_t acked = 0;
        chrono::steady_clock::time_point connected_at;
    };

    // Run one complete listener, epoll loop, and connection manager.
    void worker_loop(unsigned int worker_id);

    // Decide what to do when one descriptor becomes ready.
    void handle_event(EpollLoop& loop, ConnectionManager& connections, LoopState& state,
                      int listener_fd, int fd, unsigned int events);

    void process_completions(EpollLoop& loop, ConnectionManager& connections,
                             LoopState& state);
    void begin_drain(EpollLoop& loop, ConnectionManager& connections,
                     LoopState& state, int& listener_fd);

    // Accept every queued connection without blocking the event loop.
    void accept_clients(EpollLoop& loop, ConnectionManager& connections,
                        LoopState& state, int listener_fd);

    // Read all available client data, parse one request, and queue its response.
    void handle_client(EpollLoop& loop, ConnectionManager& connections,
                       LoopState& state, int client_fd);

    // Send as much buffered data as the socket currently accepts.
    bool flush_client(EpollLoop& loop, ConnectionManager& connections, int client_fd);

    // Remove a client from epoll and release its socket.
    void close_client(EpollLoop& loop, ConnectionManager& connections, int client_fd);

    // Binary consumer protocol (runs on the epoll worker, not the pool).
    void accept_consumers(EpollLoop& loop, ConnectionManager& connections, LoopState& state);
    void handle_consumer_event(EpollLoop& loop, ConnectionManager& connections,
                               LoopState& state, int fd, unsigned int events);
    void handle_consumer_frame(EpollLoop& loop, ConnectionManager& connections,
                               int fd, const Frame& frame);
    void pump_consumer(EpollLoop& loop, ConnectionManager& connections,
                       const LoopState& state, int fd);
    void fail_consumer(EpollLoop& loop, ConnectionManager& connections, int fd,
                       const std::string& message);
    void record_consumer(const Connection& connection);
    void notify_consumers();
    void rebuild_fleet_state();

    // A slow receiver may otherwise grow its queued echo data without bound.
    static constexpr size_t max_outbound_bytes = 1U << 20; // 1 MiB per client
    // A consumer is only refilled from the log once its queue drops below this.
    static constexpr size_t consumer_high_water_bytes = 256 * 1024;
    static constexpr size_t consumer_read_batch_bytes = 64 * 1024;

    Config config;
    unsigned int epoll_worker_count;
    atomic<bool> worker_failed{false};
    Metrics metrics;
    vector<thread> workers;
    unique_ptr<ThreadPool> thread_pool;
    MemoryMappedLog storage_log;
    FleetState fleet_state;
    Router router;

    // Each worker registers its LoopState so publishers can wake it.
    mutex loop_states_mutex;
    vector<LoopState*> loop_states;

    atomic<size_t> consumer_total{0};
    atomic<std::uint64_t> next_consumer_id{1};
    mutable mutex consumer_registry_mutex;
    std::unordered_map<std::uint64_t, ConsumerInfo> consumer_registry;
};
