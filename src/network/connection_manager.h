#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "../http/http_parser.h"

using namespace std;

struct Connection {
    int fd;
    string client_ip;
    HTTPParser parser;
    string inbound;
    string outbound;
    // The offset prevents an O(n) erase after every partial send().
    size_t outbound_offset = 0;
    bool peer_closed = false;
    bool request_in_flight = false;
    std::uint64_t generation = 0;

    Connection(int connection_fd, string ip, std::uint64_t connection_generation)
        : fd(connection_fd), client_ip(move(ip)), generation(connection_generation) {}

    size_t pending_bytes() const { return outbound.size() - outbound_offset; }
};

class ConnectionManager {
public:
    // Add a connected client to the active connection list.
    void add(int fd, string client_ip, std::uint64_t generation);

    // Remove a client after it disconnects.
    void remove(int fd);

    // Find a client, or return nullptr when it is not active.
    Connection* get(int fd);

    // Return the number of active clients.
    size_t count() const;

    std::vector<int> file_descriptors() const;

private:
    unordered_map<int, Connection> connections;
};
