#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// One position/status report from a delivery vehicle or connected device.
struct TelemetryEvent {
    std::string vehicle_id;
    double lat = 0.0;
    double lng = 0.0;
    double speed = 0.0;        // km/h
    std::string status;        // "moving" | "idle" | "offline"
    std::int64_t timestamp = 0; // Unix seconds reported by the device.
};

struct TelemetryParseResult {
    bool ok = false;
    TelemetryEvent event;
    std::string error;
};

// Limits are deliberately small: a telemetry event is a few hundred bytes.
constexpr std::size_t max_telemetry_body_bytes = 16 * 1024;
constexpr std::size_t max_vehicle_id_length = 64;
constexpr double max_speed_kmh = 400.0;

// Parse and validate a flat TelemetryEvent JSON object.
//
// Only a flat object is accepted: nested objects and arrays are rejected.
// Unknown keys with scalar values are ignored so producers can add fields
// (for example "heading") without breaking older gateways. Duplicate keys
// are rejected because their meaning would be ambiguous.
TelemetryParseResult parse_telemetry(std::string_view body);
