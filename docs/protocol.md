# Protocol

The gateway speaks two protocols on two ports:

| Role | Who | Protocol | Default port |
| --- | --- | --- | --- |
| Producer | vehicles, devices, `fleet_simulator.py` | HTTP/1.1 (`POST /publish`) | 8080 |
| Observer | dashboard, operators, scripts | HTTP/1.1 (`GET` endpoints) | 8080 |
| Consumer | `GatewayClient.subscribe_stream`, `fleet_consumer.py` | binary TCP frames | 9090 (`consumer_port`) |

Producers use HTTP because it is universally supported by devices and tools.
Consumers hold a long-lived stream, so they use a compact binary framing
with no per-message headers or text parsing. Both share the same epoll
workers, but each has its own parser (`HTTPParser` and `FrameDecoder`).

---

## 1. HTTP API

Every response carries `Connection: close`; one request is served per
connection. Every route is subject to the per-IP token bucket and returns
`429 Too Many Requests` when the bucket is empty.

| Method | Path | Response |
| --- | --- | --- |
| `POST` | `/publish` | `201` `{"offset":N}` · `400` `{"error":"<reason>"}` |
| `GET` | `/vehicles` | Latest state per vehicle and counts (see below) |
| `GET` | `/recent?count=N` | Last N records from the in-memory cache (default 100, max 1000) |
| `GET` | `/consumers` | Connected binary-protocol consumers and their offsets |
| `GET` | `/metrics` | Prometheus text exposition |
| `GET` | `/health` | `{"status":"ok"}` |
| `GET` | `/connections` | `{"connections":N}` (open HTTP connections) |
| `GET` | `/` and `/dashboard/<file>` | Dashboard files (fixed whitelist) |

### 1.1 Telemetry event (`POST /publish` body)

```text
TelemetryEvent {
    vehicle_id: string   1-64 chars of [A-Za-z0-9._-]
    lat:        number   [-90, 90]
    lng:        number   [-180, 180]
    speed:      number   [0, 400] km/h
    status:     "moving" | "idle" | "offline"
    timestamp:  integer  Unix seconds from the device, >= 0
}
```

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

Validation rules (`src/telemetry/telemetry_event.cpp`):

- The body must be one flat JSON object of at most 16 KiB. Nested objects and
  arrays are rejected.
- All six fields are required and must have the types above.
- Unknown fields with scalar values (string, number, `true`/`false`/`null`)
  are accepted and ignored, so devices can send extra data such as `heading`.
- Duplicate keys are rejected.
- A rejected event is not written to the log and does not change fleet state.
  The `400` body names the first problem, for example
  `{"error":"lat must be within [-90, 90]"}`.

The accepted body is stored byte-for-byte in the log. `201` means the record
has been written to the memory-mapped segment (see §3 for what that does and
does not guarantee).

### 1.2 `GET /vehicles`

```json
{
  "vehicles": [
    {"vehicle_id": "van-00482", "lat": 51.5074, "lng": -0.1278, "speed": 32.5,
     "status": "moving", "reported_status": "moving", "timestamp": 1726310400,
     "offset": 4096, "age_seconds": 1.2}
  ],
  "counts": {"total": 1, "active": 1, "moving": 1, "idle": 0, "offline": 0},
  "offline_after_seconds": 30
}
```

`status` is the effective status. It is `offline` when no report has arrived
for `vehicle_offline_seconds`, whatever the vehicle last reported.
`reported_status` is what the vehicle itself last sent. When two reports for
one vehicle are processed out of order, the one with the higher log offset
wins.

After a restart the view is rebuilt from the recovered recent-message cache
(the last `recent_message_capacity` records), not from the whole log. A
vehicle that has not reported within that window reappears on its next
report.

### 1.3 Metrics added for the fleet

| Series | Type | Meaning |
| --- | --- | --- |
| `gateway_telemetry_events_total` | counter | Events validated and appended |
| `gateway_telemetry_rejected_total` | counter | `POST /publish` bodies that failed validation |
| `gateway_active_vehicles` | gauge | Vehicles not offline |
| `gateway_vehicles{status=...}` | gauge | Vehicles by effective status |
| `gateway_log_end_offset` | gauge | Offset the next record will receive (= log bytes) |
| `gateway_consumers_connected` | gauge | Open consumer connections |
| `gateway_consumer_records_sent_total` | counter | `DATA` frames queued to consumers |
| `gateway_consumer_lag_bytes` | gauge | Sum over consumers of (log end - acked offset) |

The existing transport series (connections, requests, response classes,
duration histogram, pool, per-worker queues, disk bytes, uptime) are
unchanged.

---

## 2. Binary consumer protocol

### 2.1 Framing

```text
+---------------------+-----------+---------------------+
| length : u32 (BE)   | type : u8 | payload             |
+---------------------+-----------+---------------------+
  length = 1 + len(payload)
```

All integers are big-endian (network byte order). A stream is a sequence of
frames with no delimiters, and TCP may split or merge frames arbitrarily, so
both sides buffer bytes until `4 + length` bytes are available. Frames from a
consumer are limited to 1 KiB. A frame with `length = 0`, an oversized length
or an unknown type is a protocol error.

### 2.2 Frame types

| Type | Name | Direction | Payload |
| ---: | --- | --- | --- |
| 1 | `SUBSCRIBE` | client → server | empty: stream records appended from now on |
| 2 | `FETCH_FROM` | client → server | `u64 offset`: replay from `offset`, then keep streaming |
| 3 | `DATA` | server → client | `u64 offset`, `u64 next_offset`, `u64 timestamp_ns`, record bytes |
| 4 | `ACK` | client → server | `u64 next_offset` the client has finished processing |
| 5 | `ERROR` | server → client | UTF-8 reason; the server closes the connection after sending it |

A `FETCH_FROM` for offset 0 with a 120-byte record in the log produces:

```text
client: 00 00 00 09 | 02 | 00 00 00 00 00 00 00 00
server: 00 00 00 91 | 03 | 00..00 (offset 0) | 00..00 98 (next 152) | <ts> | {"vehicle_id":...}
```

### 2.3 Offsets

An offset is the record's byte position in the logical log: the sum of the
sizes (32-byte header + payload) of all earlier records. Offsets are therefore
increasing but **not consecutive**. A consumer must resume from the
`next_offset` carried in each `DATA` frame, never from `offset + 1`.
`FETCH_FROM` with an offset that is not a record boundary returns
`ERROR "offset is not a record boundary"`. An offset past the end returns
`ERROR "offset is beyond the end of the log"`.

### 2.4 Session rules

1. Send exactly one `SUBSCRIBE` or `FETCH_FROM`. A second one is an error.
2. The server sends `DATA` frames in offset order: first the stored history
   (`FETCH_FROM`), then new records as they are appended.
3. The client may send `ACK` at any time. An `ACK` beyond what has been sent is
   an error. `ACK`s are used for `/consumers` and the lag metric; the server
   does not persist them.
4. Backpressure: the server queues at most about 256 KiB per consumer and
   reads more from the log only once the socket drains. A slow consumer
   delays only itself.
5. On shutdown the server closes consumer connections. Consumers reconnect
   and resume from their own saved offset.

### 2.5 Delivery guarantee: at-least-once

The SDK (`GatewayClient.subscribe_stream`) runs the callback, then saves
`next_offset` to `offset_file`, then sends `ACK`. If the consumer process dies
after the callback but before the save, that record is delivered again on
restart. A record is never skipped. Exactly-once delivery would need the
offset save and the consumer's own side effects to commit atomically, which
is out of scope.

---

## 3. Durability boundary

`201 Created` is sent after the record is copied into the `MAP_SHARED`
mapping, i.e. into the kernel page cache. That survives a crash of the
gateway process (`kill -9`), which `tests/chaos/test_kill9.py` verifies. It
does not guarantee survival of a power loss or kernel crash, because the
gateway does not `msync`/`fsync` every record. Segments are synced when they
fill up and on clean shutdown.
