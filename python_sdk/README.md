# gateway-client

Python SDK for the IoT Delivery Fleet Monitoring gateway. Standard library only.

```bash
pip install -e python_sdk
```

```python
from gateway_client import GatewayClient

client = GatewayClient("127.0.0.1", http_port=8080, consumer_port=9090,
                       offset_file="dispatch.offset")

# Producer: HTTP POST /publish, returns the log offset.
offset = client.send_telemetry({
    "vehicle_id": "van-00482", "lat": 51.5074, "lng": -0.1278,
    "speed": 32.5, "status": "moving", "timestamp": 1726310400,
})

# Consumer: binary TCP stream. Resumes from offset_file after a restart
# (at-least-once: a record may be delivered twice, never skipped).
def handle(message):
    print(message.offset, message.json()["vehicle_id"])

client.subscribe_stream(handle)          # blocks; call client.disconnect() to stop

# Replay history up to the current end of the log.
for message in client.replay(from_offset=0):
    ...
```

Command line: `python -m gateway_client publish '<json>'`, `tail`, `vehicles`.
