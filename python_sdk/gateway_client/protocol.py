"""Binary consumer protocol framing (mirrors src/protocol/frame_codec.h).

Every frame is ``[u32 length][u8 type][payload]`` in network byte order, where
``length`` counts the type byte plus the payload. See docs/protocol.md.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import List, Optional, Tuple

SUBSCRIBE = 1
FETCH_FROM = 2
DATA = 3
ACK = 4
ERROR = 5

_LENGTH = struct.Struct(">I")
_OFFSET = struct.Struct(">Q")
_DATA_HEADER = struct.Struct(">QQQ")

# DATA frames carry whole records; this bounds memory if a stream is corrupt.
MAX_SERVER_FRAME = 64 * 1024 * 1024


def encode_frame(frame_type: int, payload: bytes = b"") -> bytes:
    return _LENGTH.pack(len(payload) + 1) + bytes([frame_type]) + payload


def encode_subscribe() -> bytes:
    return encode_frame(SUBSCRIBE)


def encode_fetch_from(offset: int) -> bytes:
    return encode_frame(FETCH_FROM, _OFFSET.pack(offset))


def encode_ack(offset: int) -> bytes:
    return encode_frame(ACK, _OFFSET.pack(offset))


@dataclass(frozen=True)
class DataFrame:
    offset: int
    next_offset: int
    timestamp_ns: int
    payload: bytes


class ProtocolError(Exception):
    """The byte stream does not follow the protocol."""


class FrameReader:
    """Reassembles frames from arbitrary TCP chunks.

    TCP is a byte stream: one ``recv()`` may hold half a frame or several
    frames, so bytes are buffered until a complete frame is available.
    """

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, chunk: bytes) -> None:
        self._buffer.extend(chunk)

    def frames(self) -> List[Tuple[int, bytes]]:
        frames = []
        while True:
            frame = self._next()
            if frame is None:
                return frames
            frames.append(frame)

    def _next(self) -> Optional[Tuple[int, bytes]]:
        if len(self._buffer) < _LENGTH.size:
            return None
        (length,) = _LENGTH.unpack_from(self._buffer)
        if length == 0 or length > MAX_SERVER_FRAME:
            raise ProtocolError(f"invalid frame length {length}")
        if len(self._buffer) < _LENGTH.size + length:
            return None
        frame_type = self._buffer[_LENGTH.size]
        payload = bytes(self._buffer[_LENGTH.size + 1:_LENGTH.size + length])
        del self._buffer[:_LENGTH.size + length]
        return frame_type, payload


def decode_data(payload: bytes) -> DataFrame:
    if len(payload) < _DATA_HEADER.size:
        raise ProtocolError("DATA frame is shorter than its header")
    offset, next_offset, timestamp_ns = _DATA_HEADER.unpack_from(payload)
    return DataFrame(offset, next_offset, timestamp_ns, payload[_DATA_HEADER.size:])
