"""Python SDK <-> binary consumer protocol, end to end."""

import json
import socket
import struct
import threading
import time

import pytest

from conftest import telemetry
from gateway_client import GatewayError
from gateway_client import protocol


def publish_many(gateway, count, prefix="van"):
    client = gateway.client()
    return [client.send_telemetry(telemetry(f"{prefix}-{index % 7}", speed=float(index % 90)))
            for index in range(count)]


def test_replay_returns_every_record_in_order(gateway):
    offsets = publish_many(gateway, 50)
    replayed = list(gateway.client().replay(from_offset=0))
    assert [message.offset for message in replayed] == offsets
    # next_offset chains each record to the following one.
    assert all(a.next_offset == b.offset for a, b in zip(replayed, replayed[1:]))
    assert replayed[-1].next_offset == gateway.metric("gateway_log_end_offset")
    assert replayed[3].json()["speed"] == 3.0


def test_replay_can_start_in_the_middle(gateway):
    offsets = publish_many(gateway, 20)
    replayed = list(gateway.client().replay(from_offset=offsets[12]))
    assert [message.offset for message in replayed] == offsets[12:]


def test_live_subscriber_receives_new_records(gateway):
    publish_many(gateway, 5)  # History before subscribing is not delivered.
    consumer = gateway.client()
    received = []
    ready = threading.Event()

    def collect(message):
        received.append(message.offset)
        if len(received) == 30:
            consumer.disconnect()

    thread = threading.Thread(target=consumer.subscribe_stream, args=(collect,), daemon=True)
    thread.start()
    # Wait until the gateway has registered the streaming consumer.
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not ready.is_set():
        consumers = gateway.get_json("/consumers")["consumers"]
        if consumers and consumers[0]["streaming"]:
            ready.set()
        time.sleep(0.02)
    assert ready.is_set()

    offsets = publish_many(gateway, 30, prefix="live")
    thread.join(timeout=10)
    assert not thread.is_alive()
    assert received == offsets


def test_consumer_acks_are_reported(gateway):
    publish_many(gateway, 10)
    end = gateway.metric("gateway_log_end_offset")
    consumer = gateway.client()

    def stop_at_end(message):
        if message.next_offset == end:
            # Give the ACK for this record time to arrive before closing.
            threading.Timer(0.3, consumer.disconnect).start()

    thread = threading.Thread(target=consumer.subscribe_stream, args=(stop_at_end, 0),
                              daemon=True)
    thread.start()
    deadline = time.monotonic() + 5
    acked = None
    while time.monotonic() < deadline:
        consumers = gateway.get_json("/consumers")["consumers"]
        if consumers and consumers[0]["acked"] == end:
            acked = consumers[0]
            break
        time.sleep(0.02)
    thread.join(timeout=5)
    assert acked is not None and acked["lag_bytes"] == 0 and acked["cursor"] == end


def test_frames_split_across_tcp_sends(gateway):
    offsets = publish_many(gateway, 3)
    with socket.create_connection(("127.0.0.1", gateway.consumer_port), timeout=5) as sock:
        frame = protocol.encode_fetch_from(0)
        # Send the request in three pieces; the gateway must reassemble it.
        for piece in (frame[:2], frame[2:7], frame[7:]):
            sock.sendall(piece)
            time.sleep(0.05)
        reader = protocol.FrameReader()
        messages = []
        while len(messages) < 3:
            chunk = sock.recv(7)  # Tiny reads force partial frames on our side.
            assert chunk, "gateway closed the stream early"
            reader.feed(chunk)
            for frame_type, payload in reader.frames():
                assert frame_type == protocol.DATA
                messages.append(protocol.decode_data(payload))
    assert [message.offset for message in messages] == offsets
    assert json.loads(messages[0].payload)["vehicle_id"] == "van-0"


def test_frame_reader_handles_any_chunking():
    stream = b"".join(
        protocol.encode_frame(protocol.DATA, struct.pack(">QQQ", i, i + 1, 0) + b"x" * i)
        for i in range(10))
    for size in (1, 3, 29, len(stream)):
        reader = protocol.FrameReader()
        decoded = []
        for start in range(0, len(stream), size):
            reader.feed(stream[start:start + size])
            decoded += [protocol.decode_data(payload) for _, payload in reader.frames()]
        assert [d.offset for d in decoded] == list(range(10))
        assert decoded[9].payload == b"x" * 9


def test_invalid_offsets_get_an_error_frame(gateway):
    publish_many(gateway, 2)
    end = int(gateway.metric("gateway_log_end_offset"))
    with pytest.raises(GatewayError, match="not a record boundary"):
        list(gateway.client().replay(from_offset=1, until_offset=end))
    with pytest.raises(GatewayError, match="beyond the end"):
        list(gateway.client().replay(from_offset=end + 100, until_offset=end + 200))


def test_protocol_violations_close_the_connection(gateway):
    with socket.create_connection(("127.0.0.1", gateway.consumer_port), timeout=5) as sock:
        sock.sendall(protocol.encode_frame(protocol.DATA, b"consumers may not send DATA"))
        reader = protocol.FrameReader()
        data = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
        reader.feed(data)
        [(frame_type, payload)] = reader.frames()
    assert frame_type == protocol.ERROR
    assert payload == b"unexpected frame from consumer"
