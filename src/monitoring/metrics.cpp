#include "metrics.h"

#include <iomanip>
#include <sstream>

Metrics::Metrics() : process_start(Clock::now()) {
    for (auto& status_class_count : response_status_classes) {
        status_class_count.store(0, std::memory_order_relaxed);
    }
}

void Metrics::connection_opened() {
    accepted_connections.fetch_add(1, std::memory_order_relaxed);
    open_connections.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::connection_closed() {
    open_connections.fetch_sub(1, std::memory_order_relaxed);
}

void Metrics::request_started() {
    requests.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::request_completed(int status_code, std::chrono::duration<double> elapsed) {
    if (status_code >= 100 && status_code < 600) {
        response_status_classes[static_cast<std::size_t>(status_code / 100 - 1)]
            .fetch_add(1, std::memory_order_relaxed);
    }
    if (status_code == 429) {
        blocked_requests.fetch_add(1, std::memory_order_relaxed);
    }

    const double seconds = elapsed.count();
    std::lock_guard<std::mutex> lock(duration_mutex);
    ++duration_observations;
    duration_sum += seconds;
    for (std::size_t index = 0; index < duration_buckets.size(); ++index) {
        if (seconds <= duration_buckets[index]) {
            ++duration_bucket_counts[index];
        }
    }
}

void Metrics::parse_error() {
    parse_errors.fetch_add(1, std::memory_order_relaxed);
}

std::size_t Metrics::active_connections() const {
    return open_connections.load(std::memory_order_relaxed);
}

std::string Metrics::prometheus_text() const {
    std::ostringstream output;
    output << std::setprecision(9);
    output << "# HELP gateway_connections_active Current open client connections.\n"
           << "# TYPE gateway_connections_active gauge\n"
           << "gateway_connections_active " << active_connections() << '\n';
    output << "# HELP gateway_connections_accepted_total Client connections accepted "
              "since startup.\n"
           << "# TYPE gateway_connections_accepted_total counter\n"
           << "gateway_connections_accepted_total "
           << accepted_connections.load(std::memory_order_relaxed) << '\n';
    output << "# HELP gateway_http_requests_total Parsed HTTP requests sent to "
              "the router.\n"
           << "# TYPE gateway_http_requests_total counter\n"
           << "gateway_http_requests_total "
           << requests.load(std::memory_order_relaxed) << '\n';
    output << "# HELP gateway_http_requests_blocked_total Requests rejected by "
              "rate limiting.\n"
           << "# TYPE gateway_http_requests_blocked_total counter\n"
           << "gateway_http_requests_blocked_total "
           << blocked_requests.load(std::memory_order_relaxed) << '\n';
    output << "# HELP gateway_http_parse_errors_total Malformed HTTP requests "
              "rejected by the parser.\n"
           << "# TYPE gateway_http_parse_errors_total counter\n"
           << "gateway_http_parse_errors_total "
           << parse_errors.load(std::memory_order_relaxed) << '\n';
    output << "# HELP gateway_http_responses_total Responses grouped by status class.\n"
           << "# TYPE gateway_http_responses_total counter\n";
    for (std::size_t index = 0; index < response_status_classes.size(); ++index) {
        output << "gateway_http_responses_total{status_class=\"" << index + 1 << "xx\"} "
               << response_status_classes[index].load(std::memory_order_relaxed) << '\n';
    }

    output << "# HELP gateway_http_request_duration_seconds Request handling "
              "duration in seconds.\n"
           << "# TYPE gateway_http_request_duration_seconds histogram\n";
    {
        std::lock_guard<std::mutex> lock(duration_mutex);
        for (std::size_t index = 0; index < duration_buckets.size(); ++index) {
            output << "gateway_http_request_duration_seconds_bucket{le=\""
                 << duration_buckets[index] << "\"} "
                 << duration_bucket_counts[index] << '\n';
        }
        output << "gateway_http_request_duration_seconds_bucket{le=\"+Inf\"} "
               << duration_observations << '\n'
               << "gateway_http_request_duration_seconds_sum " << duration_sum << '\n'
               << "gateway_http_request_duration_seconds_count "
               << duration_observations << '\n';
    }

    const std::chrono::duration<double> uptime = Clock::now() - process_start;
    output << "# HELP process_uptime_seconds Seconds since the gateway process started.\n"
           << "# TYPE process_uptime_seconds gauge\n"
           << "process_uptime_seconds " << uptime.count() << '\n';
    return output.str();
}