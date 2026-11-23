"""SIGTERM under load: the gateway drains, exits 0, and loses nothing it acknowledged.

Compared with test_kill9.py, this checks the *clean* path: accepted requests
finish, consumers are disconnected rather than reset mid-frame, and the
process exit code is 0.
"""

import socket
import threading
import time

import pytest

from conftest import telemetry
from gateway_client import GatewayError
from gateway_client import protocol

pytestmark = pytest.mark.chaos


def test_sigterm_drains_and_preserves_acknowledged_records(gateway):
    acknowledged = {}
    lock = threading.Lock()
    stop = threading.Event()

    def publisher(worker):
        client = gateway.client(timeout=2)
        index = 0
        while not stop.is_set():
            event = telemetry(f"van-{worker}", speed=float(index % 80))
            try:
                offset = client.send_telemetry(event)
            except (GatewayError, OSError):
                return
            with lock:
                acknowledged[offset] = event
            index += 1

    # A consumer is streaming when the signal arrives.
    consumer = socket.create_connection(("127.0.0.1", gateway.consumer_port), timeout=10)
    consumer.sendall(protocol.encode_subscribe())

    threads = [threading.Thread(target=publisher, args=(worker,)) for worker in range(4)]
    for thread in threads:
        thread.start()
    time.sleep(1.0)

    exit_code = gateway.stop()  # SIGTERM
    stop.set()
    for thread in threads:
        thread.join(timeout=10)
    assert exit_code == 0
    assert len(acknowledged) > 100

    # The consumer sees an orderly end of stream (EOF), not a hang.
    reader = protocol.FrameReader()
    while True:
        chunk = consumer.recv(65536)
        if not chunk:
            break
        reader.feed(chunk)
    for frame_type, _ in reader.frames():
        assert frame_type == protocol.DATA
    consumer.close()

    gateway.start()
    stored = {message.offset for message in gateway.client().replay(from_offset=0)}
    assert set(acknowledged) <= stored
