#!/usr/bin/env python3
"""Raw-socket HTTP integration tests for the running gateway."""

import os
import socket
import time
from concurrent.futures import ThreadPoolExecutor


HOST = os.environ.get("HTTP_TEST_HOST", "gateway")
PORT = int(os.environ.get("HTTP_TEST_PORT", "8080"))
SOCKET_TIMEOUT = 10
HEADER_LIMIT = 16 * 1024
BODY_LIMIT = 1 * 1024 * 1024


def connect():
    """Connect with retries while the gateway container starts."""
    deadline = time.monotonic() + 15
    last_error = None
    while time.monotonic() < deadline:
        try:
            client = socket.create_connection((HOST, PORT), timeout=SOCKET_TIMEOUT)
            client.settimeout(SOCKET_TIMEOUT)
            return client
        except OSError as error:
            last_error = error
            time.sleep(0.1)
    raise AssertionError(f"gateway did not become ready: {last_error}")


def parse_response(response):
    """Validate framing and return the status line, headers, and body."""
    separator = response.find(b"\r\n\r\n")
    assert separator != -1, "response has no header terminator"
    header_bytes = response[:separator]
    body = response[separator + 4:]
    lines = header_bytes.split(b"\r\n")
    status_line = lines[0].decode("ascii")
    headers = {}
    for line in lines[1:]:
        name, value = line.split(b":", 1)
        headers[name.decode("ascii").lower()] = value.strip().decode("ascii")
    assert "content-length" in headers, "response has no Content-Length"
    assert int(headers["content-length"]) == len(body), "incorrect Content-Length"
    return status_line, headers, body


def read_response(client):
    """Read until EOF, matching the server's Connection: close contract."""
    chunks = []
    while True:
        chunk = client.recv(8192)
        if not chunk:
            break
        chunks.append(chunk)
    return parse_response(b"".join(chunks))


def send_request(request, byte_delay=0):
    """Send one request and read the complete HTTP response."""
    with connect() as client:
        if byte_delay:
            for byte in request:
                assert client.send(bytes((byte,))) == 1
                time.sleep(byte_delay)
        else:
            client.sendall(request)
        return read_response(client)


def assert_status(request, expected_status):
    status_line, _, _ = send_request(request)
    assert status_line == expected_status, status_line


def test_health_request():
    status, headers, body = send_request(
        b"GET /health HTTP/1.1\r\nHost: gateway\r\nConnection: close\r\n\r\n"
    )
    assert status == "HTTP/1.1 200 OK"
    assert headers["content-type"] == "application/json"
    assert body == b'{"status":"ok"}'


def test_byte_by_byte_request():
    request = b"GET /health HTTP/1.1\r\nHost: gateway\r\nConnection: close\r\n\r\n"
    status, _, body = send_request(request, byte_delay=0.001)
    assert status == "HTTP/1.1 200 OK"
    assert body == b'{"status":"ok"}'


def test_fragmented_body():
    parts = [
        b"POST /missing HTTP/1.1\r\n",
        b"Host: gateway\r\n",
        b"Content-Length: 5\r\n",
        b"Connection: close\r\n\r\n",
        b"he",
        b"llo",
    ]
    with connect() as client:
        for part in parts:
            client.sendall(part)
            time.sleep(0.01)
        status, _, body = read_response(client)
    assert status == "HTTP/1.1 404 Not Found"
    assert body == b"Not Found"


def test_malformed_requests():
    requests = [
        b"GET /health\r\n\r\n",
        b"GET /health HTTP/2.0\r\nHost: gateway\r\n\r\n",
        b"GET /health HTTP/1.1\r\nBrokenHeader\r\n\r\n",
        b"GET /health HTTP/1.1\r\nContent-Length: nope\r\n\r\n",
        b"GET /health HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
    ]
    for request in requests:
        assert_status(request, "HTTP/1.1 400 Bad Request")


def test_header_normalization_and_repeated_headers():
    request = (
        b"GET /health HTTP/1.1\r\n"
        b"hOsT: gateway\r\n"
        b"X-Test: one\r\n"
        b"x-test: two\r\n"
        b"Connection: close\r\n\r\n"
    )
    status, _, body = send_request(request)
    assert status == "HTTP/1.1 200 OK"
    assert body == b'{"status":"ok"}'


def test_conflicting_content_length():
    request = (
        b"POST /missing HTTP/1.1\r\n"
        b"Content-Length: 2\r\n"
        b"Content-Length: 3\r\n\r\n"
    )
    assert_status(request, "HTTP/1.1 400 Bad Request")


def test_size_limits():
    oversized_header = (
        b"GET /health HTTP/1.1\r\nX-Fill: "
        + b"a" * HEADER_LIMIT
        + b"\r\n\r\n"
    )
    assert_status(oversized_header, "HTTP/1.1 400 Bad Request")

    oversized_body = (
        b"POST /missing HTTP/1.1\r\nContent-Length: "
        + str(BODY_LIMIT + 1).encode("ascii")
        + b"\r\n\r\n"
    )
    assert_status(oversized_body, "HTTP/1.1 400 Bad Request")


def test_routing():
    assert_status(
        b"GET /connections HTTP/1.1\r\nHost: gateway\r\n\r\n",
        "HTTP/1.1 200 OK",
    )
    assert_status(
        b"GET /missing HTTP/1.1\r\nHost: gateway\r\n\r\n",
        "HTTP/1.1 404 Not Found",
    )
    assert_status(
        b"POST /health HTTP/1.1\r\nHost: gateway\r\n\r\n",
        "HTTP/1.1 404 Not Found",
    )
    assert_status(
        b"GET /health/extra HTTP/1.1\r\nHost: gateway\r\n\r\n",
        "HTTP/1.1 404 Not Found",
    )


def test_concurrent_clients():
    request = b"GET /health HTTP/1.1\r\nHost: gateway\r\n\r\n"

    def one_client(_):
        status, _, body = send_request(request)
        assert status == "HTTP/1.1 200 OK"
        assert body == b'{"status":"ok"}'

    with ThreadPoolExecutor(max_workers=32) as executor:
        list(executor.map(one_client, range(100)))


def test_slow_reader_partial_response():
    request = b"GET /large-response HTTP/1.1\r\nHost: gateway\r\n\r\n"
    with connect() as client:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        client.sendall(request)

        response = bytearray()
        header_end = -1
        while header_end == -1:
            response.extend(client.recv(1024))
            header_end = response.find(b"\r\n\r\n")

        while True:
            chunk = client.recv(1024)
            if not chunk:
                break
            response.extend(chunk)
            time.sleep(0.001)

    status, headers, body = parse_response(bytes(response))
    assert status == "HTTP/1.1 200 OK"
    assert int(headers["content-length"]) == 512 * 1024
    assert body == b"x" * (512 * 1024)


def main():
    tests = [
        test_health_request,
        test_byte_by_byte_request,
        test_fragmented_body,
        test_malformed_requests,
        test_header_normalization_and_repeated_headers,
        test_conflicting_content_length,
        test_size_limits,
        test_routing,
        test_concurrent_clients,
        test_slow_reader_partial_response,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS all HTTP integration tests ({len(tests)})")


if __name__ == "__main__":
    main()
