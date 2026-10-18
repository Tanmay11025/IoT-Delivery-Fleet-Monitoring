#!/usr/bin/env python3
"""Drive one concurrent HTTP write stage and preserve acknowledged IDs."""

import json
import os
import selectors
import socket
import sys
import time
from pathlib import Path


def main():
    count = int(os.environ["CONCURRENCY"])
    stage = os.environ["STAGE"]
    host = os.environ.get("GATEWAY_HOST", "gateway")
    port = int(os.environ.get("GATEWAY_PORT", "8080"))
    results = Path(os.environ.get("RESULT_DIR", "/results"))
    results.mkdir(parents=True, exist_ok=True)
    ready_file = results / f"ready-{stage}"
    release_file = results / f"release-{stage}"
    ack_file = results / f"ack-{stage}.txt"

    clients = []
    connect_failures = 0
    connect_started = time.monotonic()
    for request_id in range(count):
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.settimeout(120)
        try:
            client.connect((host, port))
            clients.append((request_id, client))
        except OSError:
            connect_failures += 1
            client.close()

    connect_seconds = time.monotonic() - connect_started
    ready_file.write_text(json.dumps({"connected": len(clients),
                                      "connect_failures": connect_failures}))
    print(f"CONNECTIONS_READY {len(clients)}", flush=True)
    release_deadline = time.monotonic() + 300
    while not release_file.exists() and time.monotonic() < release_deadline:
        time.sleep(0.01)
    if not release_file.exists():
        raise TimeoutError("stage orchestrator did not release the clients")

    stage_started = time.monotonic()
    sent_at = {}
    send_failures = 0
    send_started = time.monotonic()
    for request_id, client in clients:
        if request_id == 0:
            request = b"GET /test/hold HTTP/1.1\r\nHost: gateway\r\n\r\n"
        else:
            payload = f"stage-{stage}-record-{request_id}"
            request = (f"POST /publish HTTP/1.1\r\nHost: gateway\r\n"
                       f"Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n"
                       f"{payload}").encode("ascii")
        try:
            sent_at[request_id] = time.monotonic()
            client.sendall(request)
        except OSError:
            send_failures += 1

    submit_seconds = time.monotonic() - send_started
    print(f"LOAD_IN_PROGRESS sent={len(sent_at)}", flush=True)
    response_codes = {}
    latencies = []
    hold_latency = 0.0
    latest_publish_completion = 0.0
    acknowledged = []
    read_failures = 0
    selector = selectors.DefaultSelector()
    response_buffers = {}
    socket_ids = {}
    for request_id, client in clients:
        client.setblocking(False)
        response_buffers[client] = bytearray()
        socket_ids[client] = request_id
        selector.register(client, selectors.EVENT_READ)
    response_deadline = time.monotonic() + 180
    try:
        while selector.get_map() and time.monotonic() < response_deadline:
            for key, _ in selector.select(0.5):
                client = key.fileobj
                request_id = socket_ids[client]
                try:
                    chunk = client.recv(65536)
                except BlockingIOError:
                    continue
                except OSError:
                    chunk = b""
                if chunk:
                    response_buffers[client].extend(chunk)
                    continue
                response = bytes(response_buffers[client])
                try:
                    status_line = response.split(b"\r\n", 1)[0].decode("ascii", "replace")
                    code = int(status_line.split(" ")[1])
                    response_codes[code] = response_codes.get(code, 0) + 1
                    if request_id in sent_at:
                        latency = time.monotonic() - sent_at[request_id]
                        if request_id == 0:
                            hold_latency = latency
                        else:
                            latencies.append(latency)
                            latest_publish_completion = max(latest_publish_completion,
                                                            latency)
                    if code == 201:
                        acknowledged.append(f"stage-{stage}-record-{request_id}")
                    if request_id == 0 and (code != 200 or b"held" not in response):
                        read_failures += 1
                except (ValueError, IndexError):
                    read_failures += 1
                selector.unregister(client)
                client.close()
    finally:
        for key in list(selector.get_map().values()):
            client = key.fileobj
            read_failures += 1
            selector.unregister(client)
            client.close()
        selector.close()

    ack_file.write_text("".join(payload + "\n" for payload in acknowledged))
    elapsed = time.monotonic() - stage_started
    latencies.sort()
    p95 = latencies[min(len(latencies) - 1, int(len(latencies) * 0.95))] if latencies else 0
    summary = {
        "stage": stage,
        "target_concurrency": count,
        "connected": len(clients),
        "connect_failures": connect_failures,
        "connect_seconds": connect_seconds,
        "sent": len(sent_at),
        "send_failures": send_failures,
        "request_submission_seconds": submit_seconds,
        "offered_requests_per_second": len(sent_at) / submit_seconds if submit_seconds else 0,
        "response_codes": response_codes,
        "read_failures": read_failures,
        "acknowledged_writes": len(acknowledged),
        "average_response_seconds": sum(latencies) / len(latencies) if latencies else 0,
        "p95_response_seconds": p95,
        "last_publish_response_seconds": latest_publish_completion,
        "shutdown_hold_request_seconds": hold_latency,
        "client_elapsed_seconds": elapsed,
    }
    print("STAGE_RESULT " + json.dumps(summary, sort_keys=True), flush=True)
    if connect_failures or send_failures or read_failures:
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"backend load stage failed: {error}", file=sys.stderr, flush=True)
        raise SystemExit(1)