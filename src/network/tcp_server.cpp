#include "tcp_server.h"

#include <sys/eventfd.h>



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

    Logger::info("Epoll worker " + to_string(worker_id) + " listening on port " +
                 to_string(config.port) + " with backlog " + to_string(config.backlog));
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
                    if (connection->pending_bytes() != 0) flush_client(loop, connections, fd);
                    else close_client(loop, connections, fd);
                }
            }
        },
        [&connections, &state] {
            return state.draining && state.outstanding_tasks.load(std::memory_order_relaxed) == 0 &&
                   connections.count() == 0;
        });
    if (!loop_completed && !shutdown_requested.load(memory_order_relaxed)) {
        worker_failed.store(true, memory_order_relaxed);
        shutdown_requested.store(true, memory_order_relaxed);
    }
    if (listener_fd != -1) close(listener_fd);
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

    if (fd != listener_fd && connections.get(fd) == nullptr) return;

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
        metrics.connection_opened();
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
            metrics.request_started();
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
                metrics.request_completed(503, chrono::duration<double>::zero());
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
    }
    while (!completed.empty()) {
        Completion completion = std::move(completed.front());
        completed.pop();
        metrics.request_completed(completion.status_code, completion.elapsed);
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
    for (const int fd : connections.file_descriptors()) {
        Connection* connection = connections.get(fd);
        if (connection == nullptr || connection->request_in_flight) continue;
        if (connection->pending_bytes() != 0) flush_client(loop, connections, fd);
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
    if (connections.get(client_fd) != nullptr) {
        connections.remove(client_fd);
        metrics.connection_closed();
    }
    close(client_fd);
}

void TCPServer::stop() {
    shutdown_requested.store(true, memory_order_relaxed);
}
