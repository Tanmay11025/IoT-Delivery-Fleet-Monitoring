#include "rate_limiter.h"

#include <algorithm>
#include <utility>

TokenBucket::TokenBucket(double capacity, double refill_rate)
    : capacity(capacity),
      tokens(capacity),
      refill_rate(refill_rate),
      last_refill_time(std::chrono::steady_clock::now()) {}

bool TokenBucket::consume() {
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - last_refill_time;
    tokens = std::min(capacity, tokens + elapsed.count() * refill_rate);
    last_refill_time = now;

    if (tokens < 1.0) {
        return false;
    }

    tokens -= 1.0;
    return true;
}

RateLimiter::RateLimiter(double capacity, double refill_rate)
    : capacity(capacity), refill_rate(refill_rate) {}

bool RateLimiter::check(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex);
    auto bucket = buckets.find(ip);
    if (bucket == buckets.end()) {
        bucket = buckets.emplace(ip, TokenBucket(capacity, refill_rate)).first;
    }
    return bucket->second.consume();
}
