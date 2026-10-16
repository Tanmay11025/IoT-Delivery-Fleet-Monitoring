#pragma once

#include <cstddef>
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

    static Config from_args(int argc, char* argv[]);
    static Config load_file(const std::string& path);
    void validate() const;
};