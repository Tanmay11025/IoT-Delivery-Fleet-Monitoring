"""Shared pytest fixtures: run a real gateway process on free local ports."""

from __future__ import annotations

import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional, Tuple

import pytest

REPO = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("GATEWAY_BINARY", REPO / "build" / "message_gateway"))
sys.path.insert(0, str(REPO / "python_sdk"))

from gateway_client import GatewayClient  # noqa: E402


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def telemetry(vehicle_id: str = "van-00001", **overrides) -> dict:
    event = {"vehicle_id": vehicle_id, "lat": 51.5074, "lng": -0.1278,
             "speed": 32.5, "status": "moving", "timestamp": int(time.time())}
    event.update(overrides)
    return event


class GatewayProcess:
    """Start, stop, crash and restart one gateway with a fixed storage directory."""

    def __init__(self, storage: Path, *extra_args: str) -> None:
        self.storage = storage
        self.extra_args = list(extra_args)
        self.http_port = free_port()
        self.consumer_port = free_port()
        self.process: Optional[subprocess.Popen] = None
        self.output_path = storage.parent / "gateway-output.log"

    def start(self) -> "GatewayProcess":
        if not BINARY.exists():
            pytest.fail(f"gateway binary not found at {BINARY}; build it with CMake first")
        command = [
            str(BINARY),
            "--port", str(self.http_port),
            "--consumer-port", str(self.consumer_port),
            "--epoll-workers", "2",
            "--pool-threads", "4",
            "--rate-limit-capacity", "100000",
            "--rate-limit-refill-rate", "100000",
            "--storage-directory", str(self.storage),
            "--dashboard-directory", str(REPO / "dashboard"),
            *self.extra_args,
        ]
        output = open(self.output_path, "ab")
        self.process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        output.close()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                pytest.fail(f"gateway exited early:\n{self.output_path.read_text()}")
            try:
                if self.request("GET", "/health")[0] == 200:
                    return self
            except OSError:
                time.sleep(0.05)
        pytest.fail("gateway did not become healthy within 10 s")

    def stop(self, sig: int = signal.SIGTERM, timeout: float = 30) -> int:
        if self.process is None or self.process.poll() is not None:
            return self.process.returncode if self.process else 0
        self.process.send_signal(sig)
        return self.process.wait(timeout=timeout)

    def kill9(self) -> None:
        self.stop(signal.SIGKILL)

    def restart(self) -> "GatewayProcess":
        self.stop()
        return self.start()

    def request(self, method: str, path: str, body: Optional[bytes] = None,
                timeout: float = 5) -> Tuple[int, dict, bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.http_port, timeout=timeout)
        try:
            connection.request(method, path, body=body)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def get_json(self, path: str) -> dict:
        status, _, body = self.request("GET", path)
        assert status == 200, (path, status, body)
        return json.loads(body)

    def metric(self, name: str) -> float:
        _, _, body = self.request("GET", "/metrics")
        for line in body.decode().splitlines():
            if line.startswith(name + " "):
                return float(line.split()[1])
        raise KeyError(name)

    def client(self, **kwargs) -> GatewayClient:
        return GatewayClient("127.0.0.1", self.http_port, self.consumer_port, **kwargs)


@pytest.fixture
def gateway(tmp_path):
    process = GatewayProcess(tmp_path / "log").start()
    yield process
    process.stop()
