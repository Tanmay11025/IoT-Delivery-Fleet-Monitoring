# Protocol

## Telemetry Event

Telemetry published by a delivery vehicle or connected device uses this shape:

```text
TelemetryEvent {
    vehicle_id: string
    lat: number
    lng: number
    speed: number
    status: "moving" | "idle" | "offline"
    timestamp: integer
}
```

The event is sent as JSON in the body of the existing `POST /publish` request.
No additional endpoint is required for publishing telemetry.

Example:

```json
{
  "vehicle_id": "van-00482",
  "lat": 51.5074,
  "lng": -0.1278,
  "speed": 32.5,
  "status": "moving",
  "timestamp": 1726310400
}
```
