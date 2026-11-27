#include "tcp_server.h"

#include "../core/json_util.h"
#include "../telemetry/telemetry_event.h"

#include <cerrno>
#include <charconv>
#include <sys/eventfd.h>

namespace {
// Wake an epoll worker blocked in epoll_wait().
void signal_eventfd(int fd) {
    const std::uint64_t notification = 1;
    ssize_t result;
    do {
        result = write(fd, &notification, sizeof(notification));
    } while (result == -1 && errno == EINTR);
}
}


// Signal handlers do no I/O or locking: they only update this shared flag.
// The assertion keeps that signal-handler operation lock-free on supported
// targets instead of silently falling back to an internal mutex.
static_assert(atomic<bool>::is_always_lock_free);
atomic<bool> shutdown_requested{false};

// Request a clean event-loop shutdown when Ctrl+C is pressed.
void handle_shutdown_signal(int) {
    shutdown_requested.store(true, memory_order_relaxed);
}

// Mark a descriptor nonblocking while preserving its existing flags.
bool set_nonblocking(int fd) {
    // Preserve the descriptor's existing flags before adding O_NONBLOCK.
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        return false;
    }

    // Add nonblocking mode without discarding any existing descriptor flags.
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        return false;
    }

    return true;
}

// True if no other socket is listening on the port. The workers' listeners
// use SO_REUSEPORT, which would also let a second gateway bind the same port
// and silently receive half the traffic. A probe bound without SO_REUSEPORT
// is refused by any existing listener, so it detects that case. SO_REUSEADDR
// keeps leftover TIME_WAIT connections from a restart from counting.
bool port_is_free(int port) {
    const int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe == -1) return true;  // Let the real bind report the problem.
    int option = 1;
    setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)(port));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    const bool in_use = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 &&
                        errno == EADDRINUSE;
    close(probe);
    return !in_use;
}

int create_reuseport_listener(int port, int backlog) {
    const int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd == -1) {
        perror("socket");
        return -1;
    }

    // Allow the port to be reused soon after the process is restarted.
    int option = 1;
    if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == -1 ||
        setsockopt(listener_fd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option)) == -1) {
        perror("setsockopt");
        close(listener_fd);
        return -1;
    }

    // sockaddr_in describes the local IPv4 address and TCP port to bind.
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;

    // TCP ports are transmitted in network byte order (big-endian), so the
    // host integer must be converted before it is stored in sockaddr_in.
    server_addr.sin_port = htons((uint16_t)(port));

    // INADDR_ANY listens on every local network interface.
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == -1) {
        perror("bind");
        close(listener_fd);
        return -1;
    }

    if (listen(listener_fd, backlog) == -1) {
        perror("listen");
        close(listener_fd);
        return -1;
    }

    if (!set_nonblocking(listener_fd)) {
        close(listener_fd);
        return -1;
    }

    return listener_fd;
}

TCPServer::TCPServer(Config config)
        : config(std::move(config)),
            epoll_worker_count(this->config.epoll_worker_count == 0
                                                         ? max(1u, thread::hardware_concurrency())
                                                         : this->config.epoll_worker_count),
            fleet_state(chrono::seconds(this->config.vehicle_offline_seconds)),
            router(this->config.rate_limit_capacity, this->config.rate_limit_refill_rate) {
        this->config.validate();
}

TCPServer::~TCPServer() {
    stop();
    for (thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool TCPServer::start() {
    shutdown_requested.store(false, memory_order_relaxed);
    worker_failed.store(false, memory_order_relaxed);
    metrics.set_epoll_worker_count(epoll_worker_count);
    // Refuse to share a port with another process (for example a gateway
    // left running in another terminal); checked before the log is opened
    // so two gateways never write to the same storage directory.
    for (const int port : {config.port, config.consumer_port}) {
        if (port != 0 && !port_is_free(port)) {
            cerr << "Port " << port << " is already in use by another process "
                 << "(is another gateway still running?)\n";
            return false;
        }
    }
    try {
        storage_log.open(config.storage_directory, config.storage_segment_size,
                         config.recent_message_capacity);
    } catch (const exception& exception) {
        cerr << "Unable to open persistent log: " << exception.what() << '\n';
        return false;
    }
    rebuild_fleet_state();
    try {
        thread_pool = make_unique<ThreadPool>(config.thread_pool_size,
                                              config.task_queue_capacity);
    } catch (const exception& exception) {
        cerr << "Unable to start request thread pool: " << exception.what() << '\n';
        return false;
    }
    workers.reserve(epoll_worker_count);

    try {
        for (unsigned int worker_id = 0; worker_id < epoll_worker_count; ++worker_id) {
            workers.emplace_back(&TCPServer::worker_loop, this, worker_id);
        }
    } catch (const exception& exception) {
        cerr << "Unable to start worker threads: " << exception.what() << '\n';
        worker_failed.store(true, memory_order_relaxed);
        stop();
    }

    for (thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    thread_pool->begin_shutdown();
    thread_pool->join();

    return !worker_failed.load(memory_order_relaxed);
}

void TCPServer::rebuild_fleet_state() {
    // The recent cache was refilled by log recovery, so the last N records are
    // available without another disk scan. Records written before validation
    // existed (or by tests) are simply skipped.
    const auto now_ns = static_cast<std::uint64_t>(
        chrono::duration_cast<chrono::nanoseconds>(
            chrono::system_clock::now().time_since_epoch()).count());
    std::size_t restored = 0;
    for (const Message& message : storage_log.get_recent(config.recent_message_capacity)) {
        const TelemetryParseResult parsed = parse_telemetry(message.payload);
        if (!parsed.ok) continue;
        const std::uint64_t age_ns =
            now_ns > message.timestamp_ns ? now_ns - message.timestamp_ns : 0;
        fleet_state.update(parsed.event, message.offset, chrono::nanoseconds(age_ns));
        ++restored;
    }
    if (restored != 0) {
        Logger::info("Restored fleet state from " + to_string(restored) + " recent records");
    }
}

std::uint64_t TCPServer::append_message(const std::string& payload) {
    const std::uint64_t offset = storage_log.append(payload);
    notify_consumers();
    return offset;
}

void TCPServer::notify_consumers() {
    lock_guard<mutex> lock(loop_states_mutex);
    for (LoopState* state : loop_states) {
        if (state->consumer_count.load(memory_order_relaxed) == 0) continue;
        // Only the first publisher after a drain pays for the eventfd write.
        if (!state->data_available.exchange(true, memory_order_acq_rel)) {
            signal_eventfd(state->completion_fd);
        }
    }
}

HttpResponse TCPServer::publish_telemetry(const HttpRequest& request) {
    const TelemetryParseResult parsed = parse_telemetry(request.body);
    if (!parsed.ok) {
        metrics.telemetry_rejected();
        return HttpResponse{400, "Bad Request", {{"Content-Type", "application/json"}},
                            "{\"error\":\"" + json_escape(parsed.error) + "\"}"};
    }
    const std::uint64_t offset = append_message(request.body);
    fleet_state.update(parsed.event, offset);
    metrics.telemetry_accepted();
    return HttpResponse{201, "Created", {{"Content-Type", "application/json"}},
                        "{\"offset\":" + std::to_string(offset) + "}"};
}

string TCPServer::metrics_text() const {
    const auto stats = thread_pool ? thread_pool->stats() : ThreadPool::Stats{0, 0, 0, 0};
    const FleetState::Counts counts = fleet_state.counts();
    FleetGauges fleet;
    fleet.vehicles_total = counts.total;
    fleet.vehicles_active = counts.active();
    fleet.vehicles_moving = counts.moving;
    fleet.vehicles_idle = counts.idle;
    fleet.vehicles_offline = counts.offline;
    fleet.log_end_offset = storage_log.next_offset();
    {
        lock_guard<mutex> lock(consumer_registry_mutex);
        for (const auto& entry : consumer_registry) {
            if (!entry.second.streaming) continue;
            if (fleet.log_end_offset > entry.second.acked) {
                fleet.consumer_lag_bytes += fleet.log_end_offset - entry.second.acked;
            }
        }
    }
    return metrics.prometheus_text(stats.worker_count, stats.busy_threads,
                                   stats.queue_length, storage_log.disk_usage_bytes(),
                                   stats.queue_high_water, fleet);
}

std::string TCPServer::consumers_json() const {
    const std::uint64_t end = storage_log.next_offset();
    const auto now = chrono::steady_clock::now();
    std::vector<std::pair<std::uint64_t, ConsumerInfo>> consumers;
    {
        lock_guard<mutex> lock(consumer_registry_mutex);
        consumers.assign(consumer_registry.begin(), consumer_registry.end());
    }
    std::sort(consumers.begin(), consumers.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    std::string json = "{\"log_end_offset\":" + std::to_string(end) + ",\"consumers\":[";
    bool first = true;
    for (const auto& [id, info] : consumers) {
        if (!first) json += ',';
        first = false;
        const chrono::duration<double> connected = now - info.connected_at;
        const std::uint64_t lag = info.streaming && end > info.acked ? end - info.acked : 0;
        json += "{\"id\":" + std::to_string(id) +
                ",\"client_ip\":\"" + json_escape(info.client_ip) + "\"" +
                ",\"streaming\":" + (info.streaming ? "true" : "false") +
                ",\"cursor\":" + std::to_string(info.cursor) +
                ",\"acked\":" + std::to_string(info.acked) +
                ",\"lag_bytes\":" + std::to_string(lag) +
                ",\"connected_seconds\":" + json_number(connected.count(), 1) + "}";
    }
    json += "]}";
    return json;
}

std::string TCPServer::recent_messages_json(std::size_t count) const {
    const auto messages = storage_log.get_recent(count);
    std::string json = "[";
    bool first = true;
    for (const auto& message : messages) {
        if (!first) json += ',';
        first = false;
        json += "{\"offset\":" + std::to_string(message.offset) +
                ",\"timestamp_ns\":" + std::to_string(message.timestamp_ns) +
                ",\"payload_length\":" + std::to_string(message.payload_length) +
                ",\"payload\":\"" + json_escape(message.payload) + "\"}";
    }
    json += ']';
    return json;
}

HttpResponse TCPServer::recent_messages_response(const HttpRequest& request) const {
    constexpr std::size_t default_count = 100;
    constexpr std::size_t maximum_count = 1000;
    std::size_t count = default_count;

    if (!request.query.empty()) {
        const std::string_view query(request.query);
        if (query.compare(0, 6, "count=") != 0 || query.find('&') != std::string_view::npos) {
            return HttpResponse{400, "Bad Request", {}, "Invalid recent count query"};
        }
        const std::string_view value = query.substr(6);
        if (value.empty()) {
            return HttpResponse{400, "Bad Request", {}, "Invalid recent count query"};
        }
        const auto result = std::from_chars(value.data(), value.data() + value.size(), count);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            count > maximum_count) {
            return HttpResponse{400, "Bad Request", {}, "Invalid recent count query"};
        }
    }

    return HttpResponse{200, "OK", {{"Content-Type", "application/json"}},
                        recent_messages_json(count)};
}

void TCPServer::worker_loop(unsigned int worker_id) {
    int listener_fd = create_reuseport_listener(config.port, config.backlog);
    if (listener_fd == -1) {
        worker_failed.store(true, memory_order_relaxed);
        shutdown_requested.store(true, memory_order_relaxed);
        return;
    }

    EpollLoop loop;
    ConnectionManager connections;
    LoopState state;
    state.worker_id = worker_id;
    state.completion_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (state.completion_fd == -1) {
        perror("eventfd");
        close(listener_fd);
        worker_failed.store(true, memory_order_relaxed);
        shutdown_requested.store(true, memory_order_relaxed);
        return;
    }
    if (!loop.add_fd(state.completion_fd, EPOLLIN)) {
        close(state.completion_fd);
        close(listener_fd);
        worker_failed.store(true, memory_order_relaxed);
        shutdown_requested.store(true, memory_order_relaxed);
        return;
    }
    if (!loop.add_fd(listener_fd, EPOLLIN)) {
        close(state.completion_fd);
        close(listener_fd);
        worker_failed.store(true, memory_order_relaxed);
        shutdown_requested.store(true, memory_order_relaxed);
        return;
    }
    if (config.consumer_port != 0) {
        // Every worker also owns a reuse-port listener for binary consumers,
        // so consumers are spread across workers exactly like HTTP clients.
        state.consumer_listener_fd = create_reuseport_listener(config.consumer_port,
                                                               config.backlog);
        if (state.consumer_listener_fd == -1 ||
            !loop.add_fd(state.consumer_listener_fd, EPOLLIN)) {
            if (state.consumer_listener_fd != -1) close(state.consumer_listener_fd);
            close(state.completion_fd);
            close(listener_fd);
            worker_failed.store(true, memory_order_relaxed);
            shutdown_requested.store(true, memory_order_relaxed);
            return;
        }
    }
    {
        lock_guard<mutex> lock(loop_states_mutex);
        loop_states.push_back(&state);
    }

    Logger::info("Epoll worker " + to_string(worker_id) + " listening on port " +
                 to_string(config.port) + " with backlog " + to_string(config.backlog) +
                 (config.consumer_port != 0
                      ? ", consumers on port " + to_string(config.consumer_port)
                      : string()));
    const bool loop_completed = loop.run(
        [this, &loop, &connections, &state, &listener_fd](int fd, unsigned int events) {
            handle_event(loop, connections, state, listener_fd, fd, events);
        },
        [this, &loop, &connections, &state, &listener_fd] {
            process_completions(loop, connections, state);
            if (shutdown_requested.load(memory_order_relaxed)) {
                begin_drain(loop, connections, state, listener_fd);
            }
            if (state.draining) {
                for (const int fd : connections.file_descriptors()) {
                    Connection* connection = connections.get(fd);
                    if (connection == nullptr || connection->request_in_flight) continue;
                    // Consumers hold no in-flight work; their position is
                    // their acked offset, so they can simply reconnect.
                    if (connection->kind == ConnectionKind::Consumer) {
                        close_client(loop, connections, fd);
                    } else if (connection->pending_bytes() != 0) {
                        flush_client(loop, connections, fd);
                    } else {
                        close_client(loop, connections, fd);
                    }
                }
            }
            state.consumer_count.store(connections.consumer_count(), memory_order_relaxed);
        },
        [&connections, &state] {
            return state.draining && state.outstanding_tasks.load(std::memory_order_relaxed) == 0 &&
                   connections.count() == 0;
        });
    if (!loop_completed && !shutdown_requested.load(memory_order_relaxed)) {
        worker_failed.store(true, memory_order_relaxed);
        shutdown_requested.store(true, memory_order_relaxed);
    }
    {
        // Unregister before `state` goes out of scope so publishers never
        // touch a destroyed LoopState or a closed eventfd.
        lock_guard<mutex> lock(loop_states_mutex);
        loop_states.erase(std::remove(loop_states.begin(), loop_states.end(), &state),
                          loop_states.end());
    }
    for (const int fd : connections.file_descriptors()) close_client(loop, connections, fd);
    if (listener_fd != -1) close(listener_fd);
    if (state.consumer_listener_fd != -1) close(state.consumer_listener_fd);
    close(state.completion_fd);
}

void TCPServer::handle_event(EpollLoop& loop, ConnectionManager& connections,
                             LoopState& state, int listener_fd, int fd,
                             unsigned int events) {
    if (shutdown_requested.load(memory_order_relaxed) && !state.draining) {
        begin_drain(loop, connections, state, listener_fd);
    }

    if (fd == state.completion_fd) {
        process_completions(loop, connections, state);
        return;
    }

    if (fd == state.consumer_listener_fd) {
        if (state.draining) return;
        if (events & (EPOLLERR | EPOLLHUP)) {
            worker_failed.store(true, memory_order_relaxed);
            shutdown_requested.store(true, memory_order_relaxed);
            return;
        }
        if (events & EPOLLIN) accept_consumers(loop, connections, state);
        return;
    }

    if (fd != listener_fd && connections.get(fd) == nullptr) return;

    if (fd != listener_fd && connections.get(fd)->kind == ConnectionKind::Consumer) {
        handle_consumer_event(loop, connections, state, fd, events);
        return;
    }

    if (fd == listener_fd) {
        if (state.draining) return;
        if (events & (EPOLLERR | EPOLLHUP)) {
            worker_failed.store(true, memory_order_relaxed);
            shutdown_requested.store(true, memory_order_relaxed);
            return;
        }
        if (events & EPOLLIN) {
            accept_clients(loop, connections, state, listener_fd);
        }
        return;
    }

    if (events & (EPOLLERR | EPOLLHUP)) {
        // A hard error means a response can no longer be delivered.
        close_client(loop, connections, fd);
        return;
    }

    if (events & EPOLLIN) {
        handle_client(loop, connections, state, fd);
        if (connections.get(fd) == nullptr) {
            return;
        }
    }

    Connection* connection = connections.get(fd);
    if (connection != nullptr && (events & EPOLLRDHUP)) {
        // The peer stopped writing. We may still write its queued echo back.
        connection->peer_closed = true;
    }

    if (events & EPOLLOUT) {
        flush_client(loop, connections, fd);
        if (connections.get(fd) == nullptr) {
            return;
        }
    }

    connection = connections.get(fd);
    if (connection != nullptr && !connection->request_in_flight &&
        connection->peer_closed && connection->pending_bytes() == 0) {
        close_client(loop, connections, fd);
    }
}

void TCPServer::accept_clients(EpollLoop& loop, ConnectionManager& connections,
                               LoopState& state, int listener_fd) {
    while (true) {
    if (shutdown_requested.load(memory_order_relaxed)) return;
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        const int client_fd = accept(listener_fd, reinterpret_cast<sockaddr*>(&client_addr),
                                     &client_addr_len);

        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            return;
        }

        char client_ip[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == nullptr) {
            close(client_fd);
            continue;
        }

        if (!set_nonblocking(client_fd) ||
            !loop.add_fd(client_fd, EPOLLIN | EPOLLRDHUP)) {
            close(client_fd);
            continue;
        }

        connections.add(client_fd, client_ip, state.next_generation++);
        metrics.connection_opened(state.worker_id);
    }
}

void TCPServer::handle_client(EpollLoop& loop, ConnectionManager& connections,
                              LoopState& state, int client_fd) {
    Connection* connection = connections.get(client_fd);
    if (connection == nullptr) {
        return;
    }

    char buffer[1024];
    while (true) {
        const ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            connection->peer_closed = true;
            break;
        }
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            close_client(loop, connections, client_fd);
            return;
        }

        connection->inbound.append(buffer, static_cast<size_t>(bytes_read));
        const ParseStatus status = connection->parser.feed(connection->inbound);
        connection->inbound.clear();

        if (status == ParseStatus::Error) {
            metrics.parse_error();
            const HttpResponse response{400, "Bad Request", {}, "Bad Request"};
            const string serialized = response.to_string();
            if (connection->pending_bytes() + serialized.size() > max_outbound_bytes) {
                close_client(loop, connections, client_fd);
                return;
            }
            connection->outbound.append(serialized);
            connection->peer_closed = true;
            break;
        }

        if (status == ParseStatus::Complete) {
            metrics.request_started(state.worker_id);
            const std::uint64_t generation = connection->generation;
            const HttpRequest request = connection->parser.request();
            const string client_ip = connection->client_ip;
            connection->request_in_flight = true;
            if (!loop.modify_fd(client_fd, EPOLLRDHUP)) {
                close_client(loop, connections, client_fd);
                return;
            }

            state.outstanding_tasks.fetch_add(1, memory_order_relaxed);
            const bool accepted = thread_pool->submit(
                [this, &state, client_fd, generation, request, client_ip] {
                    const auto started = Metrics::Clock::now();
                    HttpResponse response;
                    try {
                        response = router.route(request, client_ip);
                    } catch (...) {
                        response = HttpResponse{500, "Internal Server Error", {},
                                                "Internal Server Error"};
                    }
                    Completion completion{client_fd, generation, response.to_string(),
                                          response.status_code,
                                          Metrics::Clock::now() - started};
                    {
                        lock_guard<mutex> lock(state.completion_mutex);
                        state.completions.push(std::move(completion));
                        metrics.completion_queue_size(state.worker_id,
                                                      state.completions.size());
                    }
                    const std::uint64_t notification = 1;
                    ssize_t result;
                    do {
                        result = write(state.completion_fd, &notification,
                                       sizeof(notification));
                    } while (result == -1 && errno == EINTR);
                });
            if (!accepted) {
                state.outstanding_tasks.fetch_sub(1, memory_order_relaxed);
                connection->request_in_flight = false;
                const HttpResponse overloaded{503, "Service Unavailable", {},
                                              "Service Unavailable"};
                connection->outbound.append(overloaded.to_string());
                connection->peer_closed = true;
                metrics.request_completed(503, chrono::duration<double>::zero(),
                                          state.worker_id);
                flush_client(loop, connections, client_fd);
            }
            break;
        }
        if (connection->parser.error().size() > 0) {
            close_client(loop, connections, client_fd);
            return;
        }
    }

    connection = connections.get(client_fd);
    if (connection != nullptr && !connection->request_in_flight) {
        flush_client(loop, connections, client_fd);
    }
}

void TCPServer::process_completions(EpollLoop& loop, ConnectionManager& connections,
                                    LoopState& state) {
    std::uint64_t notifications;
    while (read(state.completion_fd, &notifications, sizeof(notifications)) > 0) {}

    queue<Completion> completed;
    {
        lock_guard<mutex> lock(state.completion_mutex);
        swap(completed, state.completions);
        metrics.completion_queue_size(state.worker_id, state.completions.size());
    }
    while (!completed.empty()) {
        Completion completion = std::move(completed.front());
        completed.pop();
        metrics.request_completed(completion.status_code, completion.elapsed, state.worker_id);
        Connection* connection = connections.get(completion.client_fd);
        if (connection != nullptr && connection->generation == completion.generation &&
            connection->request_in_flight) {
            connection->request_in_flight = false;
            if (connection->pending_bytes() + completion.serialized_response.size() <=
                max_outbound_bytes) {
                connection->outbound.append(completion.serialized_response);
                connection->peer_closed = true;
                flush_client(loop, connections, completion.client_fd);
            } else {
                close_client(loop, connections, completion.client_fd);
            }
        }
        state.outstanding_tasks.fetch_sub(1, memory_order_relaxed);
    }

    // Clear the flag before reading the log end, so a record appended while
    // we pump sets it again and triggers another wake-up instead of being missed.
    if (state.data_available.exchange(false, memory_order_acq_rel)) {
        for (const int fd : connections.consumer_file_descriptors()) {
            pump_consumer(loop, connections, state, fd);
        }
    }
}

void TCPServer::begin_drain(EpollLoop& loop, ConnectionManager& connections,
                            LoopState& state, int& listener_fd) {
    if (state.draining) return;
    state.draining = true;
    thread_pool->begin_shutdown();
    if (listener_fd != -1) {
        loop.remove_fd(listener_fd);
        close(listener_fd);
        listener_fd = -1;
    }
    if (state.consumer_listener_fd != -1) {
        loop.remove_fd(state.consumer_listener_fd);
        close(state.consumer_listener_fd);
        state.consumer_listener_fd = -1;
    }
    for (const int fd : connections.file_descriptors()) {
        Connection* connection = connections.get(fd);
        if (connection == nullptr || connection->request_in_flight) continue;
        if (connection->kind == ConnectionKind::Consumer) close_client(loop, connections, fd);
        else if (connection->pending_bytes() != 0) flush_client(loop, connections, fd);
        else close_client(loop, connections, fd);
    }
}

bool TCPServer::flush_client(EpollLoop& loop, ConnectionManager& connections, int client_fd) {
    Connection* connection = connections.get(client_fd);
    if (connection == nullptr) {
        return false;
    }

    while (connection->pending_bytes() != 0) {
        const ssize_t bytes_sent = send(client_fd,
                                        connection->outbound.data() + connection->outbound_offset,
                                        connection->pending_bytes(), MSG_NOSIGNAL);
        if (bytes_sent > 0) {
            connection->outbound_offset += static_cast<size_t>(bytes_sent);
            continue;
        }
        if (bytes_sent == -1 && (errno == EINTR)) {
            continue;
        }
        if (bytes_sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (loop.modify_fd(client_fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP)) {
                return true;
            }
            close_client(loop, connections, client_fd);
            return false;
        }

        close_client(loop, connections, client_fd);
        return false;
    }

    // The entire queue was sent. Release unusually large allocations so a
    // formerly slow client does not keep 1 MiB reserved forever.
    if (connection->outbound.capacity() > 64 * 1024) {
        string().swap(connection->outbound);
    } else {
        connection->outbound.clear();
    }
    connection->outbound_offset = 0;

    if (connection->peer_closed) {
        close_client(loop, connections, client_fd);
        return false;
    }

    if (loop.modify_fd(client_fd, EPOLLIN | EPOLLRDHUP)) {
        return true;
    }
    close_client(loop, connections, client_fd);
    return false;
}

void TCPServer::close_client(EpollLoop& loop, ConnectionManager& connections, int client_fd) {
    loop.remove_fd(client_fd);
    if (Connection* connection = connections.get(client_fd)) {
        if (connection->kind == ConnectionKind::Consumer) {
            {
                lock_guard<mutex> lock(consumer_registry_mutex);
                consumer_registry.erase(connection->consumer_id);
            }
            consumer_total.fetch_sub(1, memory_order_relaxed);
            metrics.consumer_disconnected();
        } else {
            metrics.connection_closed();
        }
        connections.remove(client_fd);
    }
    close(client_fd);
}

void TCPServer::accept_consumers(EpollLoop& loop, ConnectionManager& connections,
                                 LoopState& state) {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        const int client_fd = accept(state.consumer_listener_fd,
                                     reinterpret_cast<sockaddr*>(&client_addr),
                                     &client_addr_len);
        if (client_fd == -1) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept consumer");
            return;
        }

        // Reserve a slot first so concurrent workers cannot overshoot the cap.
        if (consumer_total.fetch_add(1, memory_order_relaxed) >= config.max_consumers) {
            consumer_total.fetch_sub(1, memory_order_relaxed);
            // Best effort: the socket is still blocking, and the frame is tiny.
            const std::string error = encode_error("too many consumers");
            (void)send(client_fd, error.data(), error.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            close(client_fd);
            continue;
        }

        char client_ip[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == nullptr ||
            !set_nonblocking(client_fd) || !loop.add_fd(client_fd, EPOLLIN | EPOLLRDHUP)) {
            consumer_total.fetch_sub(1, memory_order_relaxed);
            close(client_fd);
            continue;
        }

        connections.add(client_fd, client_ip, state.next_generation++, ConnectionKind::Consumer);
        Connection* connection = connections.get(client_fd);
        connection->consumer_id = next_consumer_id.fetch_add(1, memory_order_relaxed);
        {
            lock_guard<mutex> lock(consumer_registry_mutex);
            consumer_registry[connection->consumer_id] =
                ConsumerInfo{client_ip, false, 0, 0, chrono::steady_clock::now()};
        }
        state.consumer_count.store(connections.consumer_count(), memory_order_relaxed);
        metrics.consumer_connected();
    }
}

void TCPServer::handle_consumer_event(EpollLoop& loop, ConnectionManager& connections,
                                      LoopState& state, int fd, unsigned int events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        close_client(loop, connections, fd);
        return;
    }

    if (events & EPOLLIN) {
        char buffer[4096];
        while (true) {
            const ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
            if (bytes_read == 0) {
                // The consumer went away; anything still queued is undeliverable.
                close_client(loop, connections, fd);
                return;
            }
            if (bytes_read == -1) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close_client(loop, connections, fd);
                return;
            }
            Connection* connection = connections.get(fd);
            connection->frames.feed(std::string_view(buffer, static_cast<size_t>(bytes_read)));
            while (auto frame = connection->frames.next()) {
                handle_consumer_frame(loop, connections, fd, *frame);
                connection = connections.get(fd);
                if (connection == nullptr) return;
                if (connection->peer_closed) break;  // An error frame is queued.
            }
            if (connection->frames.failed()) {
                fail_consumer(loop, connections, fd, connection->frames.error());
                return;
            }
            if (connection->peer_closed) break;
        }
    }

    if (events & EPOLLRDHUP) {
        close_client(loop, connections, fd);
        return;
    }

    if (events & EPOLLOUT) {
        if (!flush_client(loop, connections, fd)) return;
    }
    pump_consumer(loop, connections, state, fd);
}

void TCPServer::handle_consumer_frame(EpollLoop& loop, ConnectionManager& connections,
                                      int fd, const Frame& frame) {
    Connection* connection = connections.get(fd);
    switch (frame.type) {
    case FrameType::Subscribe:
        if (connection->streaming) return fail_consumer(loop, connections, fd, "already streaming");
        if (!frame.payload.empty()) {
            return fail_consumer(loop, connections, fd, "SUBSCRIBE takes no payload");
        }
        connection->cursor = storage_log.next_offset();
        connection->acked = connection->cursor;
        connection->streaming = true;
        break;
    case FrameType::FetchFrom: {
        if (connection->streaming) return fail_consumer(loop, connections, fd, "already streaming");
        const auto offset = read_offset_payload(frame);
        if (!offset) return fail_consumer(loop, connections, fd, "FETCH_FROM needs a u64 offset");
        if (*offset > storage_log.next_offset()) {
            return fail_consumer(loop, connections, fd, "offset is beyond the end of the log");
        }
        // Whether the offset is a record boundary is checked on the first read.
        connection->cursor = *offset;
        connection->acked = *offset;
        connection->streaming = true;
        break;
    }
    case FrameType::Ack: {
        const auto offset = read_offset_payload(frame);
        if (!offset || !connection->streaming) {
            return fail_consumer(loop, connections, fd, "unexpected ACK");
        }
        if (*offset > connection->cursor) {
            return fail_consumer(loop, connections, fd, "ACK is ahead of delivered data");
        }
        if (*offset > connection->acked) connection->acked = *offset;
        break;
    }
    case FrameType::Data:
    case FrameType::Error:
        return fail_consumer(loop, connections, fd, "unexpected frame from consumer");
    }
    record_consumer(*connection);
}

void TCPServer::pump_consumer(EpollLoop& loop, ConnectionManager& connections,
                              const LoopState& state, int fd) {
    Connection* connection = connections.get(fd);
    if (connection == nullptr || !connection->streaming || connection->peer_closed ||
        state.draining) {
        return;
    }

    const std::uint64_t end = storage_log.next_offset();
    while (connection->cursor < end) {
        // Reclaim the already-sent prefix so a long-lived stream that never
        // fully drains cannot grow its buffer forever.
        if (connection->outbound_offset > 0 &&
            connection->outbound_offset * 2 >= connection->outbound.size()) {
            connection->outbound.erase(0, connection->outbound_offset);
            connection->outbound_offset = 0;
        }

        // Backpressure: stop refilling once enough is queued.
        std::uint64_t sent = 0;
        while (connection->pending_bytes() < consumer_high_water_bytes &&
               connection->cursor < end) {
            LogReadResult batch;
            try {
                batch = storage_log.read_from(connection->cursor, consumer_read_batch_bytes);
            } catch (const std::invalid_argument&) {
                return fail_consumer(loop, connections, fd, "offset is not a record boundary");
            } catch (const std::exception& error) {
                Logger::error(std::string("consumer read failed: ") + error.what());
                return fail_consumer(loop, connections, fd, "log read failed");
            }
            if (batch.messages.empty()) break;
            for (const Message& message : batch.messages) {
                const std::uint64_t next = message.offset + MemoryMappedLog::record_header_size +
                                           message.payload_length;
                connection->outbound += encode_data(message.offset, next, message.timestamp_ns,
                                                    message.payload);
            }
            sent += batch.messages.size();
            connection->cursor = batch.next_offset;
        }
        if (sent == 0) return;
        metrics.consumer_records_sent(sent);
        record_consumer(*connection);

        if (!flush_client(loop, connections, fd)) return;  // Socket was closed.
        connection = connections.get(fd);
        if (connection == nullptr) return;
        // The kernel buffer is full; EPOLLOUT will call us again.
        if (connection->pending_bytes() != 0) return;
    }
}

void TCPServer::fail_consumer(EpollLoop& loop, ConnectionManager& connections, int fd,
                              const std::string& message) {
    Connection* connection = connections.get(fd);
    if (connection == nullptr) return;
    connection->streaming = false;
    connection->outbound += encode_error(message);
    // flush_client closes the socket once the ERROR frame has been written.
    connection->peer_closed = true;
    flush_client(loop, connections, fd);
}

void TCPServer::record_consumer(const Connection& connection) {
    lock_guard<mutex> lock(consumer_registry_mutex);
    auto found = consumer_registry.find(connection.consumer_id);
    if (found == consumer_registry.end()) return;
    found->second.streaming = connection.streaming;
    found->second.cursor = connection.cursor;
    found->second.acked = connection.acked;
}

void TCPServer::stop() {
    shutdown_requested.store(true, memory_order_relaxed);
}
