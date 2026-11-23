#include "fleet_state.h"

#include "../core/json_util.h"

#include <algorithm>
#include <mutex>

FleetState::FleetState(std::chrono::seconds offline_after, Now now)
    : offline_after(offline_after), now(std::move(now)) {}

void FleetState::update(const TelemetryEvent& event, std::uint64_t offset,
                        std::chrono::nanoseconds age) {
    const Clock::time_point received_at =
        now() - std::chrono::duration_cast<Clock::duration>(age);
    std::unique_lock<std::shared_mutex> lock(mutex);
    auto found = vehicles.find(event.vehicle_id);
    if (found == vehicles.end()) {
        vehicles.emplace(event.vehicle_id, Entry{event, offset, received_at});
        return;
    }
    if (offset < found->second.offset) return;
    found->second = Entry{event, offset, received_at};
}

FleetState::Vehicle FleetState::describe(const Entry& entry, Clock::time_point current) const {
    Vehicle vehicle;
    vehicle.event = entry.event;
    vehicle.offset = entry.offset;
    const std::chrono::duration<double> age = current - entry.received_at;
    vehicle.age_seconds = std::max(0.0, age.count());
    const bool silent = current - entry.received_at > offline_after;
    vehicle.effective_status = silent ? "offline" : entry.event.status;
    return vehicle;
}

std::vector<FleetState::Vehicle> FleetState::snapshot() const {
    const Clock::time_point current = now();
    std::vector<Vehicle> result;
    {
        std::shared_lock<std::shared_mutex> lock(mutex);
        result.reserve(vehicles.size());
        for (const auto& entry : vehicles) result.push_back(describe(entry.second, current));
    }
    // Stable ordering keeps the dashboard table from jumping around.
    std::sort(result.begin(), result.end(), [](const Vehicle& left, const Vehicle& right) {
        return left.event.vehicle_id < right.event.vehicle_id;
    });
    return result;
}

FleetState::Counts FleetState::counts() const {
    const Clock::time_point current = now();
    Counts counts;
    std::shared_lock<std::shared_mutex> lock(mutex);
    for (const auto& entry : vehicles) {
        const Vehicle vehicle = describe(entry.second, current);
        ++counts.total;
        if (vehicle.effective_status == "moving") ++counts.moving;
        else if (vehicle.effective_status == "idle") ++counts.idle;
        else ++counts.offline;
    }
    return counts;
}

std::string FleetState::to_json() const {
    const std::vector<Vehicle> vehicles_now = snapshot();
    Counts counts;
    std::string json = "{\"vehicles\":[";
    bool first = true;
    for (const Vehicle& vehicle : vehicles_now) {
        ++counts.total;
        if (vehicle.effective_status == "moving") ++counts.moving;
        else if (vehicle.effective_status == "idle") ++counts.idle;
        else ++counts.offline;

        if (!first) json += ',';
        first = false;
        json += "{\"vehicle_id\":\"" + json_escape(vehicle.event.vehicle_id) + "\"" +
                ",\"lat\":" + json_number(vehicle.event.lat) +
                ",\"lng\":" + json_number(vehicle.event.lng) +
                ",\"speed\":" + json_number(vehicle.event.speed, 2) +
                ",\"status\":\"" + vehicle.effective_status + "\"" +
                ",\"reported_status\":\"" + json_escape(vehicle.event.status) + "\"" +
                ",\"timestamp\":" + std::to_string(vehicle.event.timestamp) +
                ",\"offset\":" + std::to_string(vehicle.offset) +
                ",\"age_seconds\":" + json_number(vehicle.age_seconds, 1) + "}";
    }
    json += "],\"counts\":{\"total\":" + std::to_string(counts.total) +
            ",\"active\":" + std::to_string(counts.active()) +
            ",\"moving\":" + std::to_string(counts.moving) +
            ",\"idle\":" + std::to_string(counts.idle) +
            ",\"offline\":" + std::to_string(counts.offline) +
            "},\"offline_after_seconds\":" + std::to_string(offline_after.count()) + "}";
    return json;
}
