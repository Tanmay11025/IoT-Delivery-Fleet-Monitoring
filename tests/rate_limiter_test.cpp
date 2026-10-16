#include "http/router.h"
#include "security/rate_limiter.h"

#include <cassert>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>

void token_bucket_allows_burst_and_refills() {
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};
    TokenBucket bucket(2.0, 10.0, [&now] { return now; });
    assert(bucket.consume());
    assert(bucket.consume());
    assert(!bucket.consume());

    now += std::chrono::milliseconds(100);
    assert(bucket.consume());
    assert(!bucket.consume());
}

void token_bucket_caps_refill_at_capacity() {
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};
    TokenBucket bucket(2.0, 10.0, [&now] { return now; });
    assert(bucket.consume());

    now += std::chrono::seconds(10);
    assert(bucket.consume());
    assert(bucket.consume());
    assert(!bucket.consume());
}

void token_bucket_does_not_refill_without_elapsed_time() {
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};
    TokenBucket bucket(1.0, 0.0, [&now] { return now; });
    assert(bucket.consume());
    now += std::chrono::hours(1);
    assert(!bucket.consume());
}

void rate_limiter_tracks_ips_independently() {
    RateLimiter limiter(1.0, 0.0);
    assert(limiter.check("192.0.2.1"));
    assert(!limiter.check("192.0.2.1"));
    assert(limiter.check("192.0.2.2"));
}

void router_rejects_before_handler_dispatch() {
    Router router(10.0, 5.0);
    int handler_calls = 0;
    router.add_route("GET", "/health", [&handler_calls](const HttpRequest&) {
        ++handler_calls;
        return HttpResponse{200, "OK", {}, "ok"};
    });

    const HttpRequest request{"GET", "/health", {}, {}};
    for (int attempt = 0; attempt < 10; ++attempt) {
        assert(router.route(request, "192.0.2.10").status_code == 200);
    }
    assert(router.route(request, "192.0.2.10").status_code == 429);
    assert(handler_calls == 10);
}

void rate_limiter_serializes_concurrent_consumption_per_ip() {
    RateLimiter limiter(20.0, 0.0);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> allowed{0};
    std::vector<std::thread> callers;
    for (int thread_index = 0; thread_index < 16; ++thread_index) {
        callers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int request = 0; request < 10; ++request) {
                if (limiter.check("192.0.2.20")) allowed.fetch_add(1);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != 16) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& caller : callers) caller.join();
    assert(allowed.load() == 20);
}

int main() {
    token_bucket_allows_burst_and_refills();
    token_bucket_caps_refill_at_capacity();
    token_bucket_does_not_refill_without_elapsed_time();
    rate_limiter_tracks_ips_independently();
    router_rejects_before_handler_dispatch();
    rate_limiter_serializes_concurrent_consumption_per_ip();
    return 0;
}