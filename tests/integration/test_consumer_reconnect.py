"""At-least-once delivery: consumers resume after crashes without gaps."""

import threading
import time

from conftest import telemetry


def all_offsets(gateway):
    return [message.offset for message in gateway.client().replay(from_offset=0)]


def test_consumer_process_crash_resumes_from_offset_file(gateway, tmp_path):
    client = gateway.client()
    for index in range(100):
        client.send_telemetry(telemetry(f"van-{index % 5}"))
    offset_file = str(tmp_path / "consumer.offset")
    seen = []

    class SimulatedCrash(Exception):
        pass

    def crash_after_40(message):
        if len(seen) == 40:
            raise SimulatedCrash()  # Dies *before* finishing record 41.
        seen.append(message.offset)

    first = gateway.client(offset_file=offset_file)
    try:
        first.subscribe_stream(crash_after_40, from_offset=0)
    except SimulatedCrash:
        pass

    # A new process with the same offset file picks up where the first stopped.
    second = gateway.client(offset_file=offset_file)
    end = gateway.metric("gateway_log_end_offset")

    def collect(message):
        seen.append(message.offset)
        if message.next_offset == end:
            second.disconnect()

    second.subscribe_stream(collect)
    expected = all_offsets(gateway)
    assert sorted(set(seen)) == expected  # No gaps...
    assert len(seen) == len(expected)     # ...and here, no duplicates either.


def test_consumer_survives_gateway_kill_and_restart(gateway, tmp_path):
    client = gateway.client()
    for index in range(50):
        client.send_telemetry(telemetry(f"van-{index % 5}"))

    first_batch_end = int(gateway.metric("gateway_log_end_offset"))
    consumer = gateway.client(offset_file=str(tmp_path / "consumer.offset"))
    seen = []
    caught_up = threading.Event()

    def collect(message):
        seen.append(message.offset)
        if message.next_offset == first_batch_end:
            caught_up.set()

    thread = threading.Thread(target=consumer.subscribe_stream, args=(collect, 0), daemon=True)
    thread.start()
    assert caught_up.wait(5)

    # The gateway dies without closing sockets cleanly, then comes back on the
    # same ports and storage. The SDK keeps retrying with backoff meanwhile.
    gateway.kill9()
    time.sleep(0.5)
    gateway.start()
    for index in range(50):
        client.send_telemetry(telemetry(f"van-{index % 5}", speed=1.0))

    expected = all_offsets(gateway)
    assert len(expected) == 100
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline and not set(expected) <= set(seen):
        time.sleep(0.05)
    consumer.disconnect()
    thread.join(timeout=5)
    assert sorted(set(seen)) == expected  # Every record delivered at least once.
