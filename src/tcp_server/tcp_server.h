
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <csignal>
#include <set>
#include <string>
#include "../core/logger.h"
#include "../epoll_loop/epoll_loop.h"

using namespace std;
#pragma once

// Set by the SIGINT handler when the user presses Ctrl+C.
void handle_sigint(int);

// Configure a file descriptor so I/O returns immediately when it would block.
bool set_nonblocking(int fd);

class TCPServer {
public:
    // Store the configuration used to create and listen on the server socket.
    TCPServer(int port, int backlog);

    // Release the server socket when the object leaves scope.
    ~TCPServer();

    // A server owns its socket and cannot be copied safely.
    TCPServer(const TCPServer&) = delete;
    TCPServer& operator=(const TCPServer&) = delete;

    // Create the socket, begin listening, and accept clients until stopped.
    bool start();

    // Close the listening socket. Calling this more than once is safe.
    void stop();

    // Accessors for server configuration (read-only)
    int get_port() const { return port; }
    int get_backlog() const { return backlog; }

private:
    // Create and bind the listening socket
    bool setup();

    // Run epoll and dispatch listening/client socket events.
    bool accept_loop();

    // Decide what to do when one descriptor becomes ready.
    void handle_event(EpollLoop& loop, int fd, unsigned int events);

    // Accept every queued connection without blocking the event loop.
    void accept_clients(EpollLoop& loop);

    // Read all available client data and echo it back.
    void handle_client(EpollLoop& loop, int client_fd);

    // Remove a client from epoll and release its socket.
    void close_client(EpollLoop& loop, int client_fd);

    // -1 means that no listening socket is currently open
    int server_fd = -1;
    int port;
    int backlog;

    // Keep client descriptors alive between epoll events.
    set<int> client_fds;
};
