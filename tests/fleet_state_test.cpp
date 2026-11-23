#include "telemetry/fleet_state.h"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "test_runner.h"

namespace {
// A controllable clock so offline detection is tested without sleeping.
struct FakeClock {
    FleetState::Clock::time_point now = FleetState::Clock::time_point{} + std::chrono::hours(1);
    FleetState::Now function() {
        return [this] { return now; };
    }
};

TelemetryEvent event(const std::string& id, const std::string& status, double lat = 1.0) {
    TelemetryEvent value;
    value.vehicle_id = id;
    value.lat = lat;
    value.lng = 2.0;
    value.speed = status == "moving" ? 30.0 : 0.0;
    value.status = status;
    value.timestamp = 1700000000;
    return value;
}
}

void fleet_state_keeps_the_latest_report_per_vehicle() {
    FakeClock clock;
    FleetState fleet(std::chrono::seconds(30), clock.function());
    fleet.update(event("van-1", "moving", 10.0), 0);
    fleet.update(event("van-2", "idle"), 100);
    fleet.update(event("van-1", "idle", 11.0), 200);

    const auto vehicles = fleet.snapshot();
    assert(vehicles.size() == 2);
    assert(vehicles[0].event.vehicle_id == "van-1");
    assert(vehicles[0].event.lat == 11.0);
    assert(vehicles[0].offset == 200);
    assert(vehicles[0].effective_status == "idle");

    const auto counts = fleet.counts();
    assert(counts.total == 2);
    assert(counts.idle == 2);
    assert(counts.moving == 0);
    assert(counts.active() == 2);
}

void fleet_state_ignores_reports_that_arrive_out_of_order() {
    FakeClock clock;
    FleetState fleet(std::chrono::seconds(30), clock.function());
    fleet.update(event("van-1", "moving", 20.0), 500);
    // A pool thread that appended earlier but finished later must not win.
    fleet.update(event("van-1", "idle", 19.0), 400);
    const auto vehicles = fleet.snapshot();
    assert(vehicles[0].event.lat == 20.0);
    assert(vehicles[0].offset == 500);
}

void fleet_state_marks_silent_vehicles_offline() {
    FakeClock clock;
    FleetState fleet(std::chrono::seconds(30), clock.function());
    fleet.update(event("van-1", "moving"), 0);
    fleet.update(event("van-2", "moving"), 100);

    clock.now += std::chrono::seconds(20);
    fleet.update(event("van-2", "moving"), 200);
    assert(fleet.counts().moving == 2);

    clock.now += std::chrono::seconds(15);  // van-1 silent for 35 s, van-2 for 15 s.
    const auto counts = fleet.counts();
    assert(counts.offline == 1);
    assert(counts.moving == 1);
    assert(counts.active() == 1);
    const auto vehicles = fleet.snapshot();
    assert(vehicles[0].effective_status == "offline");
    assert(vehicles[0].event.status == "moving");  // Reported status is preserved.
    assert(vehicles[0].age_seconds == 35.0);

    // A vehicle that reports "offline" itself is counted as offline too.
    fleet.update(event("van-3", "offline"), 300);
    assert(fleet.counts().offline == 2);
}

void fleet_state_restores_age_when_rebuilt_from_the_log() {
    FakeClock clock;
    FleetState fleet(std::chrono::seconds(30), clock.function());
    fleet.update(event("van-1", "moving"), 0, std::chrono::seconds(45));
    fleet.update(event("van-2", "moving"), 100, std::chrono::seconds(5));
    const auto counts = fleet.counts();
    assert(counts.offline == 1);
    assert(counts.moving == 1);
}

void fleet_state_json_lists_vehicles_and_counts() {
    FakeClock clock;
    FleetState fleet(std::chrono::seconds(30), clock.function());
    fleet.update(event("van-\"q\"", "moving", 51.5), 64);
    const std::string json = fleet.to_json();
    assert(json.find("\"vehicle_id\":\"van-\\\"q\\\"\"") != std::string::npos);
    assert(json.find("\"lat\":51.5,") != std::string::npos);
    assert(json.find("\"speed\":30,") != std::string::npos);
    assert(json.find("\"status\":\"moving\"") != std::string::npos);
    assert(json.find("\"offset\":64") != std::string::npos);
    assert(json.find("\"counts\":{\"total\":1,\"active\":1,\"moving\":1,\"idle\":0,"
                     "\"offline\":0}") != std::string::npos);
    assert(json.find("\"offline_after_seconds\":30") != std::string::npos);
}

void fleet_state_handles_concurrent_updates() {
    FleetState fleet(std::chrono::seconds(30));
    std::vector<std::thread> threads;
    for (int thread_id = 0; thread_id < 8; ++thread_id) {
        threads.emplace_back([&fleet, thread_id] {
            for (int index = 0; index < 1000; ++index) {
                const std::uint64_t offset =
                    static_cast<std::uint64_t>(index * 8 + thread_id);
                fleet.update(event("van-" + std::to_string(index % 50), "moving",
                                   static_cast<double>(offset)), offset);
                if (index % 100 == 0) (void)fleet.to_json();
            }
        });
    }
    for (auto& thread : threads) thread.join();

    const auto vehicles = fleet.snapshot();
    assert(vehicles.size() == 50);
    // Whatever the interleaving, each vehicle must hold its highest offset.
    for (const auto& vehicle : vehicles) {
        const int id = std::stoi(vehicle.event.vehicle_id.substr(4));
        const std::uint64_t highest = static_cast<std::uint64_t>((950 + id) * 8 + 7);
        assert(vehicle.offset == highest);
        assert(vehicle.event.lat == static_cast<double>(highest));
    }
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("fleet_state_keeps_the_latest_report_per_vehicle",
               fleet_state_keeps_the_latest_report_per_vehicle);
    runner.run("fleet_state_ignores_reports_that_arrive_out_of_order",
               fleet_state_ignores_reports_that_arrive_out_of_order);
    runner.run("fleet_state_marks_silent_vehicles_offline",
               fleet_state_marks_silent_vehicles_offline);
    runner.run("fleet_state_restores_age_when_rebuilt_from_the_log",
               fleet_state_restores_age_when_rebuilt_from_the_log);
    runner.run("fleet_state_json_lists_vehicles_and_counts",
               fleet_state_json_lists_vehicles_and_counts);
    runner.run("fleet_state_handles_concurrent_updates", fleet_state_handles_concurrent_updates);
    return runner.finish();
}
