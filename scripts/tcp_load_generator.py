#!/usr/bin/env python3
"""A small dependency-free TCP echo load generator for the gateway container.

It opens CONNECTIONS clients, sends one known payload on each, verifies the
echo, then keeps successful clients open for DURATION seconds. It is intended
to validate the gateway's TCP behavior, not HTTP throughput.
"""

import os
import selectors
import socket
import sys
import time


PAYLOAD = b"telemetry-gateway-load-check\n"
STARTUP_TIMEOUT_SECONDS = 10
ECHO_TIMEOUT_SECONDS = 30


def required_positive_int(name):
    """Read a positive integer environment variable with a clear error."""
    try:
        value = int(os.environ[name])
    except (KeyError, ValueError) as error:
        raise ValueError(f"{name} must be a positive integer") from error
    if value < 1:
        raise ValueError(f"{name} must be a positive integer")
    return value


def wait_for_gateway(address):
    """Wait for Docker DNS and the gateway listener to become available."""
    deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(address, timeout=0.5):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError("gateway did not begin accepting TCP connections")


def open_clients(address, connection_count):
    """Connect clients and send the payload before starting echo verification."""
    clients = []
    failures = 0
    for _ in range(connection_count):
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.settimeout(5)
        try:
            client.connect(address)
            client.sendall(PAYLOAD)
            client.setblocking(False)
            clients.append(client)
        except OSError:
            failures += 1
            client.close()
    return clients, failures


def verify_echoes(clients):
    """Read from many sockets at once until every payload is echoed exactly."""
    selector = selectors.DefaultSelector()
    received = {}
    for client in clients:
        selector.register(client, selectors.EVENT_READ)
        received[client] = bytearray()

    deadline = time.monotonic() + ECHO_TIMEOUT_SECONDS
    failures = 0
    pending = len(clients)
    try:
        while pending and time.monotonic() < deadline:
            for key, _ in selector.select(deadline - time.monotonic()):
                client = key.fileobj
                try:
                    chunk = client.recv(len(PAYLOAD) - len(received[client]))
                except BlockingIOError:
                    continue
                if not chunk:
                    failures += 1
                    selector.unregister(client)
                    pending -= 1
                    continue
                received[client].extend(chunk)
                if len(received[client]) == len(PAYLOAD):
                    if bytes(received[client]) != PAYLOAD:
                        failures += 1
                    selector.unregister(client)
                    pending -= 1
    finally:
        selector.close()

    # Any socket still pending timed out before producing a complete echo.
    return failures + pending


def hold_clients(clients, duration_seconds):
    """Keep verified connections open and report unexpected server closes."""
    selector = selectors.DefaultSelector()
    for client in clients:
        selector.register(client, selectors.EVENT_READ)

    failures = 0
    deadline = time.monotonic() + duration_seconds
    try:
        while time.monotonic() < deadline:
            for key, _ in selector.select(deadline - time.monotonic()):
                # No more data should arrive after the single echo. Readability
                # here means the peer closed or sent unexpected bytes.
                try:
                    data = key.fileobj.recv(1)
                except BlockingIOError:
                    continue
                if data != b"":
                    failures += 1
                else:
                    failures += 1
                selector.unregister(key.fileobj)
    finally:
        selector.close()
    return failures


def main():
    connections = required_positive_int("CONNECTIONS")
    duration = required_positive_int("DURATION")
    address = (os.environ.get("LOADTEST_HOST", "gateway"),
               required_positive_int("LOADTEST_PORT"))

    started = time.monotonic()
    wait_for_gateway(address)
    clients, failures = open_clients(address, connections)
    failures += verify_echoes(clients)
    if failures == 0:
        failures += hold_clients(clients, duration)

    for client in clients:
        client.close()

    elapsed = time.monotonic() - started
    successful = connections - failures
    print(f"connections={connections} successful={successful} failures={failures} "
          f"duration={elapsed:.2f}s")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"load test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
