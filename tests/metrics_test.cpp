#include "monitoring/metrics.h"

#include <cassert>
#include <chrono>
#include <string>
#include "test_runner.h"

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
       const std::string exposition = metrics.prometheus_text(4, 1, 2, 4096);

    assert(exposition.find("gateway_http_parse_errors_total 1\n") != std::string::npos);
    assert(exposition.find("process_uptime_seconds ") != std::string::npos);
       assert(exposition.find("gateway_log_disk_bytes 4096\n") != std::string::npos);
       assert(exposition.find(
                  "# HELP gateway_log_disk_bytes Provisioned bytes across pre-sized log segment files.\n") !=
              std::string::npos);
}

void metrics_track_telemetry_vehicles_and_consumers() {
    Metrics metrics;
    metrics.telemetry_accepted();
    metrics.telemetry_accepted();
    metrics.telemetry_rejected();
    metrics.consumer_connected();
    metrics.consumer_records_sent(3);
    assert(metrics.consumers_connected() == 1);
    // A consumer is not an HTTP connection.
    assert(metrics.active_connections() == 0);

    FleetGauges fleet;
    fleet.vehicles_total = 5;
    fleet.vehicles_active = 4;
    fleet.vehicles_moving = 3;
    fleet.vehicles_idle = 1;
    fleet.vehicles_offline = 1;
    fleet.log_end_offset = 640;
    fleet.consumer_lag_bytes = 128;
    const std::string exposition = metrics.prometheus_text(0, 0, 0, 0, 0, fleet);
    assert(exposition.find("gateway_telemetry_events_total 2\n") != std::string::npos);
    assert(exposition.find("gateway_telemetry_rejected_total 1\n") != std::string::npos);
    assert(exposition.find("gateway_active_vehicles 4\n") != std::string::npos);
    assert(exposition.find("gateway_vehicles{status=\"moving\"} 3\n") != std::string::npos);
    assert(exposition.find("gateway_vehicles{status=\"offline\"} 1\n") != std::string::npos);
    assert(exposition.find("gateway_log_end_offset 640\n") != std::string::npos);
    assert(exposition.find("gateway_consumers_connected 1\n") != std::string::npos);
    assert(exposition.find("gateway_consumer_records_sent_total 3\n") != std::string::npos);
    assert(exposition.find("gateway_consumer_lag_bytes 128\n") != std::string::npos);

    metrics.consumer_disconnected();
    assert(metrics.consumers_connected() == 0);
}

int main(int argc, char* argv[]) {
       TestRunner runner(argc, argv);
       runner.run("metrics_track_telemetry_vehicles_and_consumers",
                  metrics_track_telemetry_vehicles_and_consumers);
       runner.run("metrics_track_connection_lifecycle", metrics_track_connection_lifecycle);
       runner.run("metrics_track_requests_responses_and_duration",
                        metrics_track_requests_responses_and_duration);
       runner.run("metrics_track_parse_errors_and_uptime", metrics_track_parse_errors_and_uptime);
       return runner.finish();
}
