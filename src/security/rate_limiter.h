#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// Tracks request permits for one client using a token bucket.
class TokenBucket {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    TokenBucket(double capacity, double refill_rate,
                Clock clock = [] { return std::chrono::steady_clock::now(); });

    // Refill based on elapsed monotonic time and consume one permit if available.
    bool consume();

private:
    double capacity;
    double tokens;
    double refill_rate;
    Clock clock;
    std::chrono::steady_clock::time_point last_refill_time;
};

// Applies one token bucket to each client IP address.
class RateLimiter {
public:
    RateLimiter(double capacity, double refill_rate);

    // Return true when the IP may dispatch its next request.
    bool check(const std::string& ip);

private:
    struct BucketEntry {
        BucketEntry(double bucket_capacity, double bucket_refill_rate)
            : bucket(bucket_capacity, bucket_refill_rate) {}

        TokenBucket bucket;
        std::mutex mutex;
    };

    double capacity;
    double refill_rate;
    std::shared_mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<BucketEntry>> buckets;
};
