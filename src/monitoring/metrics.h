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

    void connection_opened();
    void connection_closed();
    void request_started();
    void request_completed(int status_code, std::chrono::duration<double> elapsed);
    void parse_error();

    std::size_t active_connections() const;
    std::string prometheus_text() const;

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

    mutable std::mutex duration_mutex;
    std::array<std::uint64_t, duration_buckets.size()> duration_bucket_counts{};
    std::uint64_t duration_observations = 0;
    double duration_sum = 0.0;
};