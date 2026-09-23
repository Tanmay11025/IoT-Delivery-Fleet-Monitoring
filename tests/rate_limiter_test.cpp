#include "http/router.h"
#include "security/rate_limiter.h"

#include <cassert>
#include <chrono>
#include <thread>

void token_bucket_allows_burst_and_refills() {
    TokenBucket bucket(2.0, 10.0);
    assert(bucket.consume());
    assert(bucket.consume());
    assert(!bucket.consume());

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    assert(bucket.consume());
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
    rate_limiter_tracks_ips_independently();
    router_rejects_before_handler_dispatch();
    return 0;
}