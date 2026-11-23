#!/usr/bin/env python3
"""Reconstruct where one vehicle was during a time window.

Replays the persistent log through the consumer protocol (FETCH_FROM) and
filters on the client by vehicle_id and the device-reported timestamp. The
gateway seeks by log offset, not by time, so this is a scan from
--from-offset (default: the start of the log) to the current end.

    python3 examples/replay_vehicle.py --vehicle van-00003 --from 2026-11-24T14:00 --to 2026-11-24T15:00
"""

from __future__ import annotations

import argparse
import math
from datetime import datetime

import _sdk_path  # noqa: F401
from gateway_client import GatewayClient


def parse_time(text: str) -> int:
    """Accept Unix seconds or an ISO-8601 local time."""
    try:
        return int(text)
    except ValueError:
        return int(datetime.fromisoformat(text).timestamp())


def haversine_km(a: dict, b: dict) -> float:
    lat1, lat2 = math.radians(a["lat"]), math.radians(b["lat"])
    dlat = lat2 - lat1
    dlng = math.radians(b["lng"] - a["lng"])
    h = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlng / 2) ** 2
    return 2 * 6371.0 * math.asin(math.sqrt(h))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vehicle", required=True)
    parser.add_argument("--from", dest="start", default=None, help="Unix seconds or ISO time")
    parser.add_argument("--to", dest="end", default=None, help="Unix seconds or ISO time")
    parser.add_argument("--from-offset", type=int, default=0)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--consumer-port", type=int, default=9090)
    args = parser.parse_args()

    start = parse_time(args.start) if args.start else None
    end = parse_time(args.end) if args.end else None
    client = GatewayClient(args.host, args.http_port, args.consumer_port)

    scanned = 0
    points = []
    for message in client.replay(from_offset=args.from_offset):
        scanned += 1
        try:
            event = message.json()
        except ValueError:
            continue
        if event.get("vehicle_id") != args.vehicle:
            continue
        if start is not None and event["timestamp"] < start:
            continue
        if end is not None and event["timestamp"] > end:
            continue
        points.append((message.offset, event))

    print(f"Scanned {scanned} records; {len(points)} belong to {args.vehicle} in the window.")
    if not points:
        return
    print(f"{'offset':>10}  {'time':19}  {'status':7}  {'speed':>6}  {'lat':>10}  {'lng':>11}")
    for offset, event in points:
        when = datetime.fromtimestamp(event["timestamp"]).isoformat(sep=" ")
        print(f"{offset:>10}  {when:19}  {event['status']:7}  {event['speed']:>6.1f}  "
              f"{event['lat']:>10.5f}  {event['lng']:>11.5f}")

    distance = sum(haversine_km(a[1], b[1]) for a, b in zip(points, points[1:]))
    speeds = [event["speed"] for _, event in points]
    duration = points[-1][1]["timestamp"] - points[0][1]["timestamp"]
    print(f"\nDistance ~{distance:.2f} km over {duration} s; "
          f"max speed {max(speeds):.1f} km/h, mean {sum(speeds) / len(speeds):.1f} km/h")


if __name__ == "__main__":
    main()
