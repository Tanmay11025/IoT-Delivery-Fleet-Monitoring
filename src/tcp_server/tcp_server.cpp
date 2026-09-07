#include "tcp_server.h"

#include <cerrno>
#include <sys/epoll.h>

// Signal handlers should do as little work as possible, so they only set a
// flag that the main server loop checks safely.
volatile sig_atomic_t shutdown_requested = false;

// Request a clean event-loop shutdown when Ctrl+C is pressed.
void handle_sigint(int) {
    shutdown_requested = true;
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

// Store the port and connection backlog used by this server.
TCPServer::TCPServer(int port, int backlog) : port(port), backlog(backlog) {}

// Stop the server and close its listening socket.
TCPServer::~TCPServer() {stop();}

// Create and bind the listening socket to the configured port.
bool TCPServer::setup() {
    // AF_INET selects IPv4 and SOCK_STREAM creates a reliable TCP socket.
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return false;
    }

    // Allow the port to be reused soon after the process is restarted.
    int option = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == -1) {
        close(server_fd);
        server_fd = -1;
        perror("setsockopt");
        return false;
    }

    // sockaddr_in describes the local IPv4 address and TCP port to bind.
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;

    // TCP ports are transmitted in network byte order (big-endian), so the
    // host integer must be converted before it is stored in sockaddr_in.
    server_addr.sin_port = htons((uint16_t)(port));

    // INADDR_ANY listens on every local network interface.
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Binding reserves the requested address and port for this socket.
    if (bind(server_fd, (struct sockaddr*)(&server_addr), sizeof(server_addr)) == -1) {
        close(server_fd);
        server_fd = -1;
        perror("bind");
        return false;
    }

    return true;
}

// Start listening, configure nonblocking mode, and enter the epoll loop.
bool TCPServer::start() {
    // Complete socket setup before putting the socket into listening mode.
    if (!setup()) {
        return false;
    }

    // listen() enables the operating system's queue for incoming connections.
    if (listen(server_fd, backlog) == -1) {
        perror("listen");
        stop();
        return false;
    }

    // Both accept() and client reads must return instead of blocking.
    if (!set_nonblocking(server_fd)) {
        stop();
        return false;
    }

    // Log that the server is now listening with both port and backlog configuration.
    Logger::info("Listening on port " + to_string(port) + " with backlog " + to_string(backlog));
    return accept_loop();
}

// Register the listening socket and dispatch all ready socket events.
bool TCPServer::accept_loop() {
    EpollLoop loop;
    if (!loop.add_fd(server_fd, EPOLLIN)) {
        return false;
    }

    // Pass each ready descriptor to the server's event handler.
    return loop.run([this, &loop](int fd, unsigned int events) {
        handle_event(loop, fd, events);
    });
}

// Handle one epoll event by choosing accept, close, or client-read logic.
void TCPServer::handle_event(EpollLoop& loop, int fd, unsigned int events) {
    if (fd == server_fd) {
        accept_clients(loop);
        return;
    }

    if (events & (EPOLLERR | EPOLLHUP)) {
        close_client(loop, fd);
        return;
    }

    if (events & EPOLLIN) {
        handle_client(loop, fd);
    }
}

// Accept every connection currently waiting in the kernel's listen queue.
void TCPServer::accept_clients(EpollLoop& loop) {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        const int client_fd = accept(server_fd, (struct sockaddr*)(&client_addr), &client_addr_len);

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

        // Each client must also be nonblocking before it enters epoll.
        if (!set_nonblocking(client_fd) || !loop.add_fd(client_fd, EPOLLIN)) {
            close(client_fd);
            continue;
        }

        client_fds.insert(client_fd);
        Logger::info("Client connected: fd=" + to_string(client_fd));
    }
}

// Read and echo all data currently available on one client socket.
void TCPServer::handle_client(EpollLoop& loop, int client_fd) {
    char buffer[1024];
    while (true) {
        const ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            close_client(loop, client_fd);
            return;
        }
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            close_client(loop, client_fd);
            return;
        }

        // Keep the existing simple echo behavior for each available chunk.
        ssize_t bytes_sent = 0;
        while (bytes_sent < bytes_read) {
            const ssize_t result = send(client_fd, buffer + bytes_sent,
                                        bytes_read - bytes_sent, MSG_NOSIGNAL);
            if (result <= 0) {
                close_client(loop, client_fd);
                return;
            }
            bytes_sent += result;
        }
    }
}

// Unregister a client socket and release its descriptor.
void TCPServer::close_client(EpollLoop& loop, int client_fd) {
    loop.remove_fd(client_fd);
    client_fds.erase(client_fd);
    close(client_fd);
}

// Close the listening socket during normal shutdown or cleanup.
void TCPServer::stop() {
    if (server_fd != -1) {
        // Closing the listening descriptor also causes future socket use to
        // stop and makes the descriptor available for reuse by the OS.
        close(server_fd);
        server_fd = -1;
    }
}


