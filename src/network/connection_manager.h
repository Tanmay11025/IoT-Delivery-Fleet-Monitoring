#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../http/http_parser.h"
#include "../protocol/frame_codec.h"

using namespace std;

// HTTP producers and binary-protocol consumers share the same epoll workers
// but are parsed by different code paths.
enum class ConnectionKind { Http, Consumer };

struct Connection {
    int fd;
    string client_ip;
    ConnectionKind kind = ConnectionKind::Http;
    HTTPParser parser;
    string inbound;
    string outbound;
    // The offset prevents an O(n) erase after every partial send().
    size_t outbound_offset = 0;
    // For HTTP: the peer stopped sending. For both kinds, a set flag means
    // "close once the queued output has been flushed".
    bool peer_closed = false;
    bool request_in_flight = false;
    std::uint64_t generation = 0;

    // Consumer-only state.
    FrameDecoder frames;
    bool streaming = false;       // SUBSCRIBE or FETCH_FROM received.
    std::uint64_t cursor = 0;     // Next log offset to send.
    std::uint64_t acked = 0;      // Offset the consumer confirmed processing up to.
    std::uint64_t consumer_id = 0;

    Connection(int connection_fd, string ip, std::uint64_t connection_generation,
               ConnectionKind connection_kind = ConnectionKind::Http)
        : fd(connection_fd), client_ip(move(ip)), kind(connection_kind),
          generation(connection_generation) {}

    size_t pending_bytes() const { return outbound.size() - outbound_offset; }
};

class ConnectionManager {
public:
    // Add a connected client to the active connection list.
    void add(int fd, string client_ip, std::uint64_t generation,
             ConnectionKind kind = ConnectionKind::Http);

    // Remove a client after it disconnects.
    void remove(int fd);

    // Find a client, or return nullptr when it is not active.
    Connection* get(int fd);

    // Return the number of active clients.
    size_t count() const;

    std::vector<int> file_descriptors() const;

    // Consumer connections only, so new records can be pushed without
    // scanning every HTTP connection on the worker.
    std::vector<int> consumer_file_descriptors() const;
    size_t consumer_count() const;

private:
    unordered_map<int, Connection> connections;
    unordered_set<int> consumers;
};
