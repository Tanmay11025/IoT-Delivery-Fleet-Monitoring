#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

class Metrics {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Metrics();

    void set_epoll_worker_count(std::size_t count);
    void connection_opened(std::size_t worker_id = 0);
    void connection_closed();
    void request_started(std::size_t worker_id = 0);
    void request_completed(int status_code, std::chrono::duration<double> elapsed,
                           std::size_t worker_id = 0);
    void completion_queue_size(std::size_t worker_id, std::size_t size);
    void parse_error();

    std::size_t active_connections() const;
    std::string prometheus_text(std::size_t pool_workers = 0,
                                std::size_t busy_threads = 0,
                                std::size_t queue_length = 0,
                                std::uint64_t log_disk_bytes = 0,
                                std::size_t queue_high_water = 0) const;

private:
    static constexpr std::array<double, 11> duration_buckets{
        0.005, 0.01, 0.025, 0.05, 0.1, 0.25,
        0.5,   1.0,  2.5,   5.0,  10.0};

    const TimePoint process_start;
    std::atomic<std::uint64_t> accepted_connections{0};
    std::atomic<std::size_t> open_connections{0};
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> blocked_requests{0};
    std::atomic<std::uint64_t> parse_errors{0};
    std::array<std::atomic<std::uint64_t>, 5> response_status_classes{};

    struct EpollWorkerMetrics {
        std::atomic<std::uint64_t> accepted_connections{0};
        std::atomic<std::uint64_t> requests{0};
        std::atomic<std::uint64_t> completed_requests{0};
        std::atomic<std::size_t> completion_queue_depth{0};
        std::atomic<std::size_t> completion_queue_high_water{0};
    };
    std::array<EpollWorkerMetrics, 256> epoll_workers{};
    std::atomic<std::size_t> epoll_worker_count{0};

    mutable std::mutex duration_mutex;
    std::array<std::uint64_t, duration_buckets.size()> duration_bucket_counts{};
    std::uint64_t duration_observations = 0;
    double duration_sum = 0.0;
};