## IoT Delivery Fleet Monitoring

IoT Delivery Fleet Monitoring aims to provide a live tracking and telemetry
platform for delivery vehicles and connected IoT sensors.

The project is intended to ingest location and status updates, preserve the
fleet's history, and make current information available to fleet operators and
downstream services in real time.

## Project Goals

- Track the current location of delivery vehicles and connected devices.
- Monitor vehicle status such as moving, idle, or offline.
- Collect telemetry including speed, timestamps, and device identity.
- Preserve historical fleet activity so routes and events can be replayed later.
- Provide a live fleet view for monitoring vehicle positions and health.
- Support alerts and operational decisions based on incoming telemetry.
- Remain reliable during high traffic, connection failures, and process crashes.
- Provide realistic fleet simulation for testing and demonstrations.

## Planned Fleet Event

The platform is designed around telemetry events containing information such as:

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

## Intended Outcome

The finished project will demonstrate a complete fleet-monitoring workflow:
devices and simulated vehicles publish telemetry, the platform preserves and
streams that data, and a fleet operations view shows the latest positions,
statuses, and important events.

