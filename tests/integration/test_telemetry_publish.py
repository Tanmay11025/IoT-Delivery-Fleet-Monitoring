"""POST /publish validation, fleet state, and the observability endpoints."""

import json

import pytest

from conftest import telemetry
from gateway_client import ValidationError


def test_valid_event_is_stored_and_visible(gateway):
    client = gateway.client()
    event = telemetry("van-00482", lat=51.5, lng=-0.12, speed=30.0)
    assert client.send_telemetry(event) == 0

    vehicles = gateway.get_json("/vehicles")
    assert vehicles["counts"] == {"total": 1, "active": 1, "moving": 1, "idle": 0, "offline": 0}
    [vehicle] = vehicles["vehicles"]
    assert vehicle["vehicle_id"] == "van-00482"
    assert (vehicle["lat"], vehicle["lng"], vehicle["speed"]) == (51.5, -0.12, 30.0)
    assert vehicle["offset"] == 0

    [record] = gateway.get_json("/recent?count=1")
    assert json.loads(record["payload"]) == event
    assert gateway.metric("gateway_telemetry_events_total") == 1
    assert gateway.metric("gateway_active_vehicles") == 1


@pytest.mark.parametrize("body, reason", [
    (b"", "body must be a JSON object"),
    (b"not json", "body must be a JSON object"),
    (json.dumps(telemetry(lat=123.0)).encode(), "lat must be within [-90, 90]"),
    (json.dumps(telemetry(status="parked")).encode(),
     "status must be one of moving, idle, offline"),
    (json.dumps({"vehicle_id": "van-1"}).encode(), "missing field: lat"),
    (b'{"vehicle_id":"van-1","position":{"lat":1}}',
     "nested values are not supported (field position)"),
])
def test_invalid_events_are_rejected_with_a_reason(gateway, body, reason):
    status, headers, response = gateway.request("POST", "/publish", body)
    assert status == 400
    assert headers["Content-Type"] == "application/json"
    assert json.loads(response) == {"error": reason}
    # Nothing reached the log or the fleet view.
    assert gateway.metric("gateway_log_end_offset") == 0
    assert gateway.get_json("/vehicles")["counts"]["total"] == 0
    assert gateway.metric("gateway_telemetry_rejected_total") == 1


def test_sdk_surfaces_validation_errors(gateway):
    with pytest.raises(ValidationError, match="speed must be within"):
        gateway.client().send_telemetry(telemetry(speed=-5))


def test_latest_event_wins_per_vehicle(gateway):
    client = gateway.client()
    client.send_telemetry(telemetry("van-1", lat=10.0, status="moving"))
    client.send_telemetry(telemetry("van-2", lat=20.0, status="idle", speed=0))
    last = client.send_telemetry(telemetry("van-1", lat=11.0, status="idle", speed=0))
    vehicles = {v["vehicle_id"]: v for v in gateway.get_json("/vehicles")["vehicles"]}
    assert vehicles["van-1"]["lat"] == 11.0
    assert vehicles["van-1"]["status"] == "idle"
    assert vehicles["van-1"]["offset"] == last
    assert gateway.get_json("/vehicles")["counts"]["idle"] == 2


def test_fleet_state_is_rebuilt_after_restart(gateway):
    client = gateway.client()
    for index in range(5):
        client.send_telemetry(telemetry(f"van-{index}", lat=50.0 + index))
    before = gateway.get_json("/vehicles")["vehicles"]

    gateway.restart()
    after = gateway.get_json("/vehicles")["vehicles"]
    assert [(v["vehicle_id"], v["lat"], v["offset"]) for v in after] == \
           [(v["vehicle_id"], v["lat"], v["offset"]) for v in before]
