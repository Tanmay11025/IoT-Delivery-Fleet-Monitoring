#pragma once

#include <cstddef>
#include <unordered_map>

struct Connection {
    int fd;
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
    std::size_t count() const;

private:
    std::unordered_map<int, Connection> connections;
};
