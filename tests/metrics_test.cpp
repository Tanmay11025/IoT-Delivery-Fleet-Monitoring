#include "monitoring/metrics.h"

#include <cassert>
#include <chrono>
#include <string>

void metrics_track_connection_lifecycle() {
    Metrics metrics;
    assert(metrics.active_connections() == 0);

    metrics.connection_opened();
    assert(metrics.active_connections() == 1);
    const std::string exposition = metrics.prometheus_text();
    assert(exposition.find("gateway_connections_active 1\n") !=
           std::string::npos);
    assert(exposition.find("gateway_connections_accepted_total 1\n") !=
           std::string::npos);

    metrics.connection_closed();
    assert(metrics.active_connections() == 0);
}

void metrics_track_requests_responses_and_duration() {
    Metrics metrics;
    metrics.request_started();
    metrics.request_completed(200, std::chrono::duration<double>(0.02));
    metrics.request_started();
    metrics.request_completed(429, std::chrono::duration<double>(0.5));

    const std::string exposition = metrics.prometheus_text();
    assert(exposition.find("gateway_http_requests_total 2\n") != std::string::npos);
    assert(exposition.find("gateway_http_requests_blocked_total 1\n") !=
           std::string::npos);
    assert(exposition.find("gateway_http_responses_total{status_class=\"2xx\"} 1\n") !=
           std::string::npos);
    assert(exposition.find("gateway_http_responses_total{status_class=\"4xx\"} 1\n") !=
           std::string::npos);
    assert(exposition.find(
               "gateway_http_request_duration_seconds_bucket{le=\"0.025\"} 1\n") !=
           std::string::npos);
    assert(exposition.find(
               "gateway_http_request_duration_seconds_bucket{le=\"+Inf\"} 2\n") !=
           std::string::npos);
    assert(exposition.find("gateway_http_request_duration_seconds_count 2\n") !=
           std::string::npos);
    assert(exposition.find("gateway_http_request_duration_seconds_sum 0.52\n") !=
           std::string::npos);
}

void metrics_track_parse_errors_and_uptime() {
    Metrics metrics;
    metrics.parse_error();
    const std::string exposition = metrics.prometheus_text();

    assert(exposition.find("gateway_http_parse_errors_total 1\n") != std::string::npos);
    assert(exposition.find("process_uptime_seconds ") != std::string::npos);
}

int main() {
    metrics_track_connection_lifecycle();
    metrics_track_requests_responses_and_duration();
    metrics_track_parse_errors_and_uptime();
    return 0;
}
