#include "connection_manager.h"

// Store a new client connection by its file descriptor.
void ConnectionManager::add(int fd) {
    connections[fd] = Connection{fd};
}

// Erase a client connection from the active connection list.
void ConnectionManager::remove(int fd) {
    connections.erase(fd);
}

// Return the connection belonging to fd when it is still active.
Connection* ConnectionManager::get(int fd) {
    auto connection = connections.find(fd);
    if (connection == connections.end()) {
        return nullptr;
    }
    return &connection->second;
}

// Return how many client connections are currently active.
std::size_t ConnectionManager::count() const {
    return connections.size();
}
