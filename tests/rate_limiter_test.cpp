#include "http/router.h"
#include "security/rate_limiter.h"

#include <cassert>
#include <chrono>

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
    Router router;
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

int main() {
    token_bucket_allows_burst_and_refills();
    token_bucket_caps_refill_at_capacity();
    token_bucket_does_not_refill_without_elapsed_time();
    rate_limiter_tracks_ips_independently();
    router_rejects_before_handler_dispatch();
    return 0;
}