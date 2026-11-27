"""A second gateway must not share the ports of one that is already running.

The listeners use SO_REUSEPORT, which would otherwise let a second gateway
bind the same port and silently receive part of the traffic (and write into
the same kind of storage directory).
"""

import subprocess

import pytest

from conftest import BINARY, REPO, free_port


def run_second_gateway(http_port: int, consumer_port: int, storage) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(BINARY), "--port", str(http_port), "--consumer-port", str(consumer_port),
         "--storage-directory", str(storage), "--dashboard-directory", str(REPO / "dashboard")],
        capture_output=True, text=True, timeout=10)


@pytest.mark.parametrize("busy", ["http", "consumer"])
def test_second_gateway_refuses_a_port_in_use(gateway, tmp_path, busy):
    http_port = gateway.http_port if busy == "http" else free_port()
    consumer_port = gateway.consumer_port if busy == "consumer" else free_port()
    storage = tmp_path / "second-log"

    result = run_second_gateway(http_port, consumer_port, storage)

    assert result.returncode != 0
    busy_port = http_port if busy == "http" else consumer_port
    assert f"Port {busy_port} is already in use" in result.stdout + result.stderr
    assert not storage.exists(), "the refused gateway must not open its log"
    # The running gateway is unaffected.
    assert gateway.request("GET", "/health")[0] == 200
