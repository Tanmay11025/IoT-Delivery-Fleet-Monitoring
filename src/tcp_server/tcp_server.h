
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <sstream>
#include <csignal>
#include <string>
#include "../core/logger.h"

using namespace std;
#pragma once

// Set by the SIGINT handler when the user presses Ctrl+C.
void handle_sigint(int);

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

    // Accept clients one at a time and pass each one to handle_client()
    bool accept_loop();

    // -1 means that no listening socket is currently open
    int server_fd = -1;
    int port;
    int backlog;
};

// handles client
class ClientConnection {
    int fd;
public: 
    ClientConnection(int f) : fd(f) {}
    ~ClientConnection() {close(fd);}

    // A connection owns its socket and cannot be copied safely.
    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    // Initialize and handle the client connection; logs connection info and processes I/O.
    void ClientInitiate();
    void handle_client();
};
