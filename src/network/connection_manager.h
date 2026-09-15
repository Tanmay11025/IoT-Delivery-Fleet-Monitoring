#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include "../http/http_parser.h"

using namespace std;

struct Connection {
    int fd;
    HTTPParser parser;
    string inbound;
    string outbound;
    // The offset prevents an O(n) erase after every partial send().
    size_t outbound_offset = 0;
    bool peer_closed = false;

    explicit Connection(int connection_fd) : fd(connection_fd) {}

    size_t pending_bytes() const { return outbound.size() - outbound_offset; }
};

class ConnectionManager {
public:
    // Add a connected client to the active connection list.
    void add(int fd);

    // Remove a client after it disconnects.
    void remove(int fd);

    // Find a client, or return nullptr when it is not active.
    Connection* get(int fd);

    // Return the number of active clients.
    size_t count() const;

private:
    unordered_map<int, Connection> connections;
};
