#include "tcp_server.h"

// Signal handlers should do as little work as possible, so they only set a
// flag that the main server loop checks safely.
volatile sig_atomic_t shutdown_requested = false;

void handle_sigint(int) {
    shutdown_requested = true;
}

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

TCPServer::TCPServer(int port, int backlog) : port(port), backlog(backlog) {}

TCPServer::~TCPServer() {stop();}

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

    // Log that the server is now listening with both port and backlog configuration.
    Logger::info("Listening on port " + to_string(port) + " with backlog " + to_string(backlog));
    return accept_loop();
}

bool TCPServer::accept_loop() {
    // Each accepted client is handled synchronously before accepting the next.
    // The loop ends after Ctrl+C sets shutdown_requested.
    while (server_fd != -1 && !shutdown_requested) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);

        // accept() removes the next waiting connection and returns a new
        // descriptor; server_fd remains available for future clients.
        const int client_fd = accept(server_fd, (struct sockaddr*)(&client_addr), &client_addr_len);

        // Check if accept failed
        if (client_fd == -1) {
            perror("accept");
            return false;
        }

        // Create ClientConnection object; destructor closes fd automatically when leaving scope
        ClientConnection client(client_fd);
        client.ClientInitiate();
    }

    return true;
}

void TCPServer::stop() {
    if (server_fd != -1) {
        // Closing the listening descriptor also causes future socket use to
        // stop and makes the descriptor available for reuse by the OS.
        close(server_fd);
        server_fd = -1;
    }
}

// implementation of ClientConnection

// Initialize the client connection by validating the file descriptor, logging the connection,
// and passing control to handle_client() for echo protocol processing.
void ClientConnection::ClientInitiate() {
    if (fd == -1) {
        perror("accept");
        return;
    }

    // Log each new client connection with its file descriptor for debugging and monitoring.
    Logger::info("Client connected: fd=" + to_string(fd));
    handle_client();
}

void ClientConnection::handle_client() {
    char buffer[1024];
    while (true) {
        // recv() may return fewer bytes than the client sent; each call is one
        // available chunk, not necessarily one complete application message.
        const ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes_read == 0) {
            // A zero-byte read means the client closed its side of the socket.
            return;
        }
        if (bytes_read == -1) {
            perror("recv");
            return;
        }

        // send() can write only part of the buffer, so keep sending until the
        // entire chunk received above has been echoed back.
        ssize_t bytes_sent = 0;
        while (bytes_sent < bytes_read) {
            ssize_t result = send(fd, buffer + bytes_sent, bytes_read - bytes_sent, MSG_NOSIGNAL);
            if (result == -1) {
                perror("send");
                return;
            }
            if (result == 0) {
                return;
            }
            bytes_sent += result;
        }
    }
}

