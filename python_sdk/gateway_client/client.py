"""GatewayClient: publish telemetry over HTTP, consume it over the binary protocol."""

from __future__ import annotations

import http.client
import json
import os
import socket
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Dict, Iterator, Mapping, Optional

from . import protocol


class GatewayError(Exception):
    """The gateway rejected a request or reported a protocol error."""


class ValidationError(GatewayError):
    """POST /publish returned 400: the event failed validation."""


class RateLimitedError(GatewayError):
    """POST /publish returned 429: this client exceeded its token bucket."""


@dataclass(frozen=True)
class Message:
    """One record delivered to a consumer."""

    offset: int        # Log offset of this record.
    next_offset: int   # Offset to resume from after processing this record.
    timestamp_ns: int  # Time the gateway appended the record.
    payload: bytes

    def json(self) -> Dict[str, Any]:
        return json.loads(self.payload)


class GatewayClient:
    """Client for the fleet gateway.

    Producers call :meth:`send_telemetry` (HTTP ``POST /publish``). Consumers
    call :meth:`subscribe_stream`, which uses the binary TCP protocol and, when
    ``offset_file`` is set, resumes after a restart from the last record the
    callback finished processing. Delivery is at-least-once: a record can be
    delivered again if the process stops after the callback but before the
    offset is saved.
    """

    def __init__(self, host: str = "127.0.0.1", http_port: int = 8080,
                 consumer_port: int = 9090, *, timeout: float = 5.0,
                 offset_file: Optional[str] = None) -> None:
        self.host = host
        self.http_port = http_port
        self.consumer_port = consumer_port
        self.timeout = timeout
        self.offset_file = offset_file
        self._stop = threading.Event()
        self._socket_lock = threading.Lock()
        self._socket: Optional[socket.socket] = None

    # ------------------------------------------------------------------ HTTP

    def _request(self, method: str, path: str, body: Optional[bytes] = None):
        connection = http.client.HTTPConnection(self.host, self.http_port, timeout=self.timeout)
        try:
            headers = {"Content-Type": "application/json"} if body is not None else {}
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def send_telemetry(self, event: Mapping[str, Any]) -> int:
        """Publish one telemetry event and return the log offset it was stored at."""
        body = json.dumps(event, separators=(",", ":")).encode("utf-8")
        status, response = self._request("POST", "/publish", body)
        if status == 201:
            return int(json.loads(response)["offset"])
        if status == 400:
            try:
                reason = json.loads(response)["error"]
            except (ValueError, KeyError):
                reason = response.decode("utf-8", "replace")
            raise ValidationError(reason)
        if status == 429:
            raise RateLimitedError("rate limited by the gateway")
        raise GatewayError(f"publish failed with HTTP {status}")

    def vehicles(self) -> Dict[str, Any]:
        """Return the gateway's latest-state-per-vehicle view (GET /vehicles)."""
        status, response = self._request("GET", "/vehicles")
        if status != 200:
            raise GatewayError(f"/vehicles returned HTTP {status}")
        return json.loads(response)

    def log_end_offset(self) -> int:
        """Return the offset the next published record will receive."""
        status, response = self._request("GET", "/metrics")
        if status != 200:
            raise GatewayError(f"/metrics returned HTTP {status}")
        for line in response.decode("utf-8").splitlines():
            if line.startswith("gateway_log_end_offset "):
                return int(float(line.split()[1]))
        raise GatewayError("gateway_log_end_offset missing from /metrics")

    # -------------------------------------------------------------- consumer

    def subscribe_stream(self, callback: Callable[[Message], None],
                         from_offset: Optional[int] = None, *, reconnect: bool = True,
                         max_backoff: float = 5.0) -> None:
        """Stream records to ``callback`` until :meth:`disconnect` is called.

        The start position is, in order of preference: ``from_offset``, the
        offset saved in ``offset_file``, or the current end of the log (only
        new records). After each callback returns, the record's
        ``next_offset`` is saved and acknowledged, so a reconnect continues
        exactly after the last processed record: no gaps, possibly a
        duplicate. If ``callback`` raises, the exception propagates and that
        record is not marked as processed.
        """
        position = from_offset if from_offset is not None else self._load_offset()
        self._stop.clear()
        backoff = 0.2
        while not self._stop.is_set():
            try:
                if position is None:
                    position = self.log_end_offset()
                for message in self._stream(position):
                    callback(message)
                    position = message.next_offset
                    self._save_offset(position)
                    self._send(protocol.encode_ack(position))
                    backoff = 0.2
            except (OSError, ConnectionError, GatewayConnectionLost, http.client.HTTPException):
                if self._stop.is_set():
                    break
                if not reconnect:
                    raise
                time.sleep(backoff)
                backoff = min(backoff * 2, max_backoff)
            finally:
                self._close_socket()

    def replay(self, from_offset: int = 0,
               until_offset: Optional[int] = None) -> Iterator[Message]:
        """Yield stored records from ``from_offset`` up to ``until_offset``.

        ``until_offset`` defaults to the end of the log at the time of the
        call, so the iterator finishes instead of waiting for new data.
        """
        end = self.log_end_offset() if until_offset is None else until_offset
        if from_offset >= end:
            return
        self._stop.clear()
        try:
            for message in self._stream(from_offset):
                yield message
                if message.next_offset >= end:
                    return
        finally:
            self._close_socket()

    def disconnect(self) -> None:
        """Stop :meth:`subscribe_stream` (safe to call from another thread)."""
        self._stop.set()
        self._close_socket(shutdown=True)

    def __enter__(self) -> "GatewayClient":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.disconnect()

    # --------------------------------------------------------------- helpers

    def _stream(self, position: int) -> Iterator[Message]:
        sock = socket.create_connection((self.host, self.consumer_port), timeout=self.timeout)
        # A short receive timeout lets the loop notice disconnect() promptly.
        sock.settimeout(0.5)
        with self._socket_lock:
            self._socket = sock
        sock.sendall(protocol.encode_fetch_from(position))
        reader = protocol.FrameReader()
        while not self._stop.is_set():
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                raise GatewayConnectionLost("gateway closed the stream")
            reader.feed(chunk)
            for frame_type, payload in reader.frames():
                if frame_type == protocol.DATA:
                    data = protocol.decode_data(payload)
                    yield Message(data.offset, data.next_offset, data.timestamp_ns, data.payload)
                elif frame_type == protocol.ERROR:
                    raise GatewayError(payload.decode("utf-8", "replace"))
                else:
                    raise protocol.ProtocolError(f"unexpected frame type {frame_type}")

    def _send(self, frame: bytes) -> None:
        with self._socket_lock:
            sock = self._socket
        if sock is not None:
            sock.sendall(frame)

    def _close_socket(self, shutdown: bool = False) -> None:
        with self._socket_lock:
            sock, self._socket = self._socket, None
        if sock is None:
            return
        if shutdown:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        sock.close()

    def _load_offset(self) -> Optional[int]:
        if not self.offset_file or not os.path.exists(self.offset_file):
            return None
        with open(self.offset_file, "r", encoding="ascii") as handle:
            text = handle.read().strip()
        return int(text) if text else None

    def _save_offset(self, offset: int) -> None:
        if not self.offset_file:
            return
        # Write-then-rename so a crash never leaves a half-written offset.
        temporary = self.offset_file + ".tmp"
        with open(temporary, "w", encoding="ascii") as handle:
            handle.write(str(offset))
        os.replace(temporary, self.offset_file)


class GatewayConnectionLost(ConnectionError):
    """The consumer connection closed; subscribe_stream() reconnects."""
