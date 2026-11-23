"""kill -9 the gateway while vehicles are publishing; nothing acknowledged is lost.

What this proves: a record whose POST /publish returned 201 survives a
*process* crash (SIGKILL), because mmap writes land in the kernel page cache
before the response is sent. It does not prove survival of a power loss:
the gateway does not msync/fsync each record, so that needs a different test
(and a durability setting this project deliberately does not have).
"""

import json
import threading

import pytest

from conftest import telemetry
from gateway_client import GatewayError

pytestmark = pytest.mark.chaos

PUBLISHERS = 4
EVENTS_PER_PUBLISHER = 250
KILL_AFTER = 600


def test_acknowledged_records_survive_kill9(gateway):
    acknowledged = {}  # offset -> event
    lock = threading.Lock()
    killed = threading.Event()

    def publisher(worker):
        client = gateway.client(timeout=2)
        for index in range(EVENTS_PER_PUBLISHER):
            event = telemetry(f"van-{worker}-{index % 10}", lat=50.0 + index / 1000.0,
                              speed=float(index % 80))
            try:
                offset = client.send_telemetry(event)
            except (GatewayError, OSError):
                return  # The gateway is gone; this request was never acknowledged.
            with lock:
                acknowledged[offset] = event
                if len(acknowledged) >= KILL_AFTER and not killed.is_set():
                    killed.set()
                    gateway.kill9()  # Mid-stream: other publishers are still sending.

    threads = [threading.Thread(target=publisher, args=(worker,)) for worker in range(PUBLISHERS)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=30)
    assert killed.is_set(), "the gateway was never killed"
    assert len(acknowledged) >= KILL_AFTER

    gateway.start()  # Recovery scans the segments and truncates any torn tail.

    stored = {message.offset: json.loads(message.payload)
              for message in gateway.client().replay(from_offset=0)}
    missing = [offset for offset in acknowledged if offset not in stored]
    assert missing == [], f"{len(missing)} acknowledged records were lost"
    for offset, event in acknowledged.items():
        assert stored[offset] == event

    # The fleet view is rebuilt from the log: every vehicle's position is at
    # least as new as its last acknowledged report.
    last_acked = {}
    for offset, event in sorted(acknowledged.items()):
        last_acked[event["vehicle_id"]] = offset
    vehicles = {v["vehicle_id"]: v for v in gateway.get_json("/vehicles")["vehicles"]}
    for vehicle_id, offset in last_acked.items():
        assert vehicles[vehicle_id]["offset"] >= offset
        assert vehicles[vehicle_id]["lat"] == stored[vehicles[vehicle_id]["offset"]]["lat"]

    # The log keeps accepting writes after recovery, with increasing offsets.
    end_before = gateway.metric("gateway_log_end_offset")
    assert gateway.client().send_telemetry(telemetry("van-after-crash")) == end_before
