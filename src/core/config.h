#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct Config {
    int port = 8080;
    int backlog = 128;
    unsigned int epoll_worker_count = 0;
    unsigned int thread_pool_size = 4;
    std::size_t task_queue_capacity = 1024;
    std::string log_path;
    double rate_limit_capacity = 10.0;
    double rate_limit_refill_rate = 5.0;
    std::string storage_directory = "data/log";
    std::uint64_t storage_segment_size = 64ULL * 1024ULL * 1024ULL;
    std::size_t recent_message_capacity = 1024;
    // Binary consumer protocol listener; 0 disables it.
    int consumer_port = 9090;
    std::size_t max_consumers = 64;
    // Directory holding index.html, app.js, ... served at / and /dashboard/.
    std::string dashboard_directory = "dashboard";
    // A vehicle with no report for this long is shown as offline.
    unsigned int vehicle_offline_seconds = 30;

    static Config from_args(int argc, char* argv[]);
    static Config load_file(const std::string& path);
    void validate() const;
};