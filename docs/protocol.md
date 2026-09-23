# Protocol

## Protocol Split

Producers speak HTTP to this gateway and are parsed by its HTTP parser.
Consumers planned for Week 9 will speak a separate binary TCP protocol. They
will not speak HTTP. Keeping these protocols separate lets the producer-facing
API evolve independently from the efficient consumer-facing stream format.

## Observability Endpoints

- `GET /health` returns `{"status":"ok"}` as JSON.
- `GET /connections` returns the current process-wide active connection count
  as JSON, for example `{"connections":3}`.

The connection count is maintained with an atomic counter because each worker
owns a separate connection map. The endpoint reports the aggregate count
without reading another worker's map.

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
