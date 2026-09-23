#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

// Tracks request permits for one client using a token bucket.
class TokenBucket {
public:
    TokenBucket(double capacity, double refill_rate);

    // Refill based on elapsed monotonic time and consume one permit if available.
    bool consume();

private:
    double capacity;
    double tokens;
    double refill_rate;
    std::chrono::steady_clock::time_point last_refill_time;
};

// Applies one token bucket to each client IP address.
class RateLimiter {
public:
    // Defaults allow a short burst and five requests per second afterward.
    explicit RateLimiter(double capacity = 10.0, double refill_rate = 5.0);

    // Return true when the IP may dispatch its next request.
    bool check(const std::string& ip);

private:
    double capacity;
    double refill_rate;
    std::mutex mutex;
    std::unordered_map<std::string, TokenBucket> buckets;
};
