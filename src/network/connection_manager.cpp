#include "connection_manager.h"

// Store a new client connection by its file descriptor.
void ConnectionManager::add(int fd, string client_ip, std::uint64_t generation,
                            ConnectionKind kind) {
    connections.insert_or_assign(fd, Connection(fd, move(client_ip), generation, kind));
    if (kind == ConnectionKind::Consumer) consumers.insert(fd);
    else consumers.erase(fd);
}

// Erase a client connection from the active connection list.
void ConnectionManager::remove(int fd) {
    connections.erase(fd);
    consumers.erase(fd);
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
size_t ConnectionManager::count() const {
    return connections.size();
}

std::vector<int> ConnectionManager::file_descriptors() const {
    std::vector<int> descriptors;
    descriptors.reserve(connections.size());
    for (const auto& entry : connections) descriptors.push_back(entry.first);
    return descriptors;
}

std::vector<int> ConnectionManager::consumer_file_descriptors() const {
    return std::vector<int>(consumers.begin(), consumers.end());
}

size_t ConnectionManager::consumer_count() const {
    return consumers.size();
}
