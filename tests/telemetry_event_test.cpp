#include "telemetry/telemetry_event.h"

#include <cassert>
#include <string>
#include "test_runner.h"

namespace {
const std::string valid_event =
    R"({"vehicle_id":"van-00482","lat":51.5074,"lng":-0.1278,"speed":32.5,)"
    R"("status":"moving","timestamp":1726310400})";

std::string rejection(const std::string& body) {
    const TelemetryParseResult result = parse_telemetry(body);
    assert(!result.ok);
    return result.error;
}
}

void telemetry_accepts_a_valid_event() {
    const TelemetryParseResult result = parse_telemetry(valid_event);
    assert(result.ok);
    assert(result.event.vehicle_id == "van-00482");
    assert(result.event.lat == 51.5074);
    assert(result.event.lng == -0.1278);
    assert(result.event.speed == 32.5);
    assert(result.event.status == "moving");
    assert(result.event.timestamp == 1726310400);
}

void telemetry_accepts_whitespace_order_and_unknown_scalar_fields() {
    const TelemetryParseResult result = parse_telemetry(
        " {\n  \"timestamp\": 0, \"status\": \"idle\", \"speed\": 0,\n"
        "  \"heading\": 270, \"note\": \"at depot\", \"door_open\": false,\n"
        "  \"lng\": 180, \"lat\": -90, \"vehicle_id\": \"truck_7.a\" } ");
    assert(result.ok);
    assert(result.event.vehicle_id == "truck_7.a");
    assert(result.event.status == "idle");
    assert(result.event.lat == -90.0);
    assert(result.event.lng == 180.0);
}

void telemetry_decodes_string_escapes() {
    const TelemetryParseResult result = parse_telemetry(
        R"({"vehicle_id":"van-1","lat":1,"lng":2,"speed":3,"status":"moving",)"
        R"("timestamp":4})");
    assert(result.ok);
    assert(result.event.vehicle_id == "van-1");
    assert(result.event.status == "moving");
}

void telemetry_rejects_missing_and_mistyped_fields() {
    assert(rejection("") == "body must be a JSON object");
    assert(rejection("[]") == "body must be a JSON object");
    assert(rejection("{}") == "missing field: vehicle_id");
    assert(rejection(R"({"vehicle_id":"v","lat":1,"lng":2,"speed":3,"status":"idle"})") ==
           "missing field: timestamp");
    assert(rejection(R"({"vehicle_id":"v","lat":"1","lng":2,"speed":3,"status":"idle",)"
                     R"("timestamp":1})") == "lat must be a number");
    assert(rejection(R"({"vehicle_id":7,"lat":1,"lng":2,"speed":3,"status":"idle",)"
                     R"("timestamp":1})") == "vehicle_id must be a string");
    assert(rejection(R"({"vehicle_id":"v","lat":1,"lng":2,"speed":3,"status":"idle",)"
                     R"("timestamp":1.5})") == "timestamp must be an integer");
    assert(rejection(R"({"vehicle_id":"v","lat":1,"lng":2,"speed":null,"status":"idle",)"
                     R"("timestamp":1})") == "speed has the wrong type");
}

void telemetry_rejects_out_of_range_values() {
    const auto event = [](const std::string& field, const std::string& value) {
        std::string body = R"({"vehicle_id":"v","lat":1,"lng":2,"speed":3,"status":"idle",)"
                           R"("timestamp":1})";
        const std::string key = "\"" + field + "\":";
        const auto start = body.find(key) + key.size();
        const auto end = body.find_first_of(",}", start);
        return body.replace(start, end - start, value);
    };
    assert(rejection(event("lat", "90.0001")) == "lat must be within [-90, 90]");
    assert(rejection(event("lng", "-180.5")) == "lng must be within [-180, 180]");
    assert(rejection(event("speed", "-1")) == "speed must be within [0, 400] km/h");
    assert(rejection(event("speed", "401")) == "speed must be within [0, 400] km/h");
    assert(rejection(event("status", "\"parked\"")) ==
           "status must be one of moving, idle, offline");
    assert(rejection(event("vehicle_id", "\"\"")) ==
           "vehicle_id must be 1-64 characters of [A-Za-z0-9._-]");
    assert(rejection(event("vehicle_id", "\"van 1\"")) ==
           "vehicle_id must be 1-64 characters of [A-Za-z0-9._-]");
    assert(rejection(event("vehicle_id", "\"" + std::string(65, 'a') + "\"")) ==
           "vehicle_id must be 1-64 characters of [A-Za-z0-9._-]");
    assert(rejection(event("timestamp", "-1")) == "timestamp is out of range");
}

void telemetry_rejects_malformed_json() {
    assert(rejection(valid_event + "x") == "unexpected data after the JSON object");
    assert(rejection(R"({"vehicle_id":"v","vehicle_id":"w"})") == "duplicate field: vehicle_id");
    assert(rejection(R"({"vehicle_id":"v",})") == "expected a quoted field name");
    assert(rejection(R"({"vehicle_id" "v"})") == "expected ':' after field vehicle_id");
    assert(rejection(R"({"position":{"lat":1}})") ==
           "nested values are not supported (field position)");
    assert(rejection(R"({"lat":01})") == "expected ',' or '}' after field lat");
    assert(rejection(R"({"lat":1.})") == "invalid number for lat");
    assert(rejection(R"({"lat":1e999})") == "invalid number for lat");
    assert(rejection("{\"vehicle_id\":\"a\nb\"}") == "invalid string for vehicle_id");
    assert(rejection(R"({"vehicle_id":"\ud800"})") == "invalid string for vehicle_id");
    assert(rejection(R"({"vehicle_id":"v")") == "expected ',' or '}' after field vehicle_id");
    assert(rejection(std::string(max_telemetry_body_bytes + 1, ' ')).find("exceeds") !=
           std::string::npos);
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("telemetry_accepts_a_valid_event", telemetry_accepts_a_valid_event);
    runner.run("telemetry_accepts_whitespace_order_and_unknown_scalar_fields",
               telemetry_accepts_whitespace_order_and_unknown_scalar_fields);
    runner.run("telemetry_decodes_string_escapes", telemetry_decodes_string_escapes);
    runner.run("telemetry_rejects_missing_and_mistyped_fields",
               telemetry_rejects_missing_and_mistyped_fields);
    runner.run("telemetry_rejects_out_of_range_values", telemetry_rejects_out_of_range_values);
    runner.run("telemetry_rejects_malformed_json", telemetry_rejects_malformed_json);
    return runner.finish();
}
