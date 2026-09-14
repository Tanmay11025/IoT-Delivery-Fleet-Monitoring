#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

using namespace std;

struct Connection {
    int fd;
    // Bytes waiting to be echoed. The offset prevents an O(n) erase after
    // every partial send().
    string outbound;
    size_t outbound_offset = 0;
    bool peer_closed = false;

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
