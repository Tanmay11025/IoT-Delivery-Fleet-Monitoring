#!/usr/bin/env python3
"""A dispatch-monitor consumer built on the SDK.

Streams telemetry over the binary protocol, keeps the last status of every
van, and prints an alert when a van exceeds the speed limit or has been
silent for longer than the offline window. With --offset-file it resumes
after a restart from the last record it processed (at-least-once delivery).

    python3 examples/fleet_consumer.py --speed-limit 50 --offset-file dispatch.offset
"""

from __future__ import annotations

import argparse
import threading
import time
from typing import Dict

import _sdk_path  # noqa: F401
from gateway_client import GatewayClient, Message


class DispatchMonitor:
    def __init__(self, speed_limit: float, offline_after: float) -> None:
        self.speed_limit = speed_limit
        self.offline_after = offline_after
        self.last_seen: Dict[str, float] = {}
        self.speeding: set = set()
        self.offline: set = set()
        self.processed = 0
        self.lock = threading.Lock()

    def handle(self, message: Message) -> None:
        try:
            event = message.json()
            vehicle, speed = event["vehicle_id"], float(event["speed"])
        except (ValueError, KeyError):
            return  # Not a telemetry record.
        with self.lock:
            self.processed += 1
            self.last_seen[vehicle] = time.monotonic()
            if vehicle in self.offline:
                self.offline.discard(vehicle)
                alert(f"{vehicle} is back online (offset {message.offset})")
            if speed > self.speed_limit and vehicle not in self.speeding:
                self.speeding.add(vehicle)
                alert(f"{vehicle} OVERSPEED {speed:.1f} km/h > {self.speed_limit:.0f} "
                      f"(offset {message.offset})")
            elif speed <= self.speed_limit:
                self.speeding.discard(vehicle)

    def check_offline(self) -> None:
        now = time.monotonic()
        with self.lock:
            for vehicle, seen in self.last_seen.items():
                if now - seen > self.offline_after and vehicle not in self.offline:
                    self.offline.add(vehicle)
                    alert(f"{vehicle} OFFLINE: no report for {now - seen:.0f} s")


def alert(text: str) -> None:
    print(f"{time.strftime('%H:%M:%S')}  {text}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--consumer-port", type=int, default=9090)
    parser.add_argument("--speed-limit", type=float, default=50.0)
    parser.add_argument("--offline-after", type=float, default=30.0)
    parser.add_argument("--offset-file", default=None,
                        help="persist progress here and resume from it on restart")
    parser.add_argument("--from-offset", type=int, default=None)
    args = parser.parse_args()

    monitor = DispatchMonitor(args.speed_limit, args.offline_after)
    client = GatewayClient(args.host, args.http_port, args.consumer_port,
                           offset_file=args.offset_file)

    def watchdog() -> None:
        while True:
            time.sleep(5)
            monitor.check_offline()

    threading.Thread(target=watchdog, daemon=True).start()
    print("Dispatch monitor running (Ctrl+C to stop)", flush=True)
    try:
        client.subscribe_stream(monitor.handle, from_offset=args.from_offset)
    except KeyboardInterrupt:
        client.disconnect()
    print(f"Processed {monitor.processed} records")


if __name__ == "__main__":
    main()
