#!/usr/bin/env python3
"""Simulate a delivery fleet publishing telemetry through the SDK.

Each van drives a random walk around a city centre, stops for "deliveries"
(idle), and occasionally goes silent for a while so the dashboard can show it
turning offline. This is a demo/load *simulation*: every van publishes from
this one process, i.e. from one client IP. Run the gateway with
config.demo.ini, otherwise the per-IP rate limiter (correctly) rejects most
of the traffic, and the 429 count printed below shows it.

    python3 examples/fleet_simulator.py --vehicles 25
"""

from __future__ import annotations

import argparse
import math
import random
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field

import _sdk_path  # noqa: F401
from gateway_client import GatewayClient, GatewayError, RateLimitedError, ValidationError

KM_PER_DEGREE = 111.32


@dataclass
class Van:
    vehicle_id: str
    lat: float
    lng: float
    heading: float = field(default_factory=lambda: random.uniform(0, 2 * math.pi))
    speed: float = 0.0
    state: str = "moving"
    ticks_left: int = 0

    def step(self, seconds: float, center: tuple, radius_km: float) -> None:
        self.ticks_left -= 1
        if self.state == "moving":
            if self.ticks_left <= 0 and random.random() < 0.08:
                self.state, self.ticks_left = "idle", random.randint(4, 15)
            elif random.random() < 0.004:
                # Lost signal: stop publishing for long enough to be marked offline.
                self.state, self.ticks_left = "silent", random.randint(35, 70)
            target = random.uniform(25, 55)
            if random.random() < 0.01:
                target = random.uniform(75, 95)  # Occasional speeding for the alert demo.
            self.speed += (target - self.speed) * 0.3
            self.heading += random.gauss(0, 0.25)
            # Steer back toward the centre when drifting outside the service area.
            north_km = (center[0] - self.lat) * KM_PER_DEGREE
            east_km = (center[1] - self.lng) * KM_PER_DEGREE * math.cos(math.radians(self.lat))
            if math.hypot(north_km, east_km) > radius_km:
                self.heading = math.atan2(east_km, north_km) + random.gauss(0, 0.3)
            distance_km = self.speed * seconds / 3600.0
            self.lat += distance_km * math.cos(self.heading) / KM_PER_DEGREE
            self.lng += distance_km * math.sin(self.heading) / (
                KM_PER_DEGREE * math.cos(math.radians(self.lat)))
        elif self.state == "idle":
            self.speed = 0.0
            if self.ticks_left <= 0:
                self.state, self.ticks_left = "moving", random.randint(10, 40)
        elif self.state == "silent" and self.ticks_left <= 0:
            self.state, self.ticks_left = "moving", random.randint(10, 40)

    def event(self) -> dict:
        return {
            "vehicle_id": self.vehicle_id,
            "lat": round(self.lat, 6),
            "lng": round(self.lng, 6),
            "speed": round(max(self.speed, 0.0), 1),
            "status": "idle" if self.state == "idle" else "moving",
            "timestamp": int(time.time()),
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--vehicles", type=int, default=25)
    parser.add_argument("--interval", type=float, default=1.0, help="seconds between reports")
    parser.add_argument("--duration", type=float, default=0, help="stop after N seconds (0 = run forever)")
    parser.add_argument("--center", default="51.5074,-0.1278", help="lat,lng of the service area")
    parser.add_argument("--radius-km", type=float, default=6.0)
    parser.add_argument("--workers", type=int, default=8, help="concurrent HTTP requests")
    parser.add_argument("--seed", type=int, default=None)
    args = parser.parse_args()

    random.seed(args.seed)
    center = tuple(float(part) for part in args.center.split(","))
    vans = []
    for index in range(args.vehicles):
        angle, distance = random.uniform(0, 2 * math.pi), random.uniform(0, args.radius_km)
        vans.append(Van(
            vehicle_id=f"van-{index + 1:05d}",
            lat=center[0] + distance * math.cos(angle) / KM_PER_DEGREE,
            lng=center[1] + distance * math.sin(angle) / (
                KM_PER_DEGREE * math.cos(math.radians(center[0]))),
            ticks_left=random.randint(0, 20)))

    client = GatewayClient(args.host, args.http_port)
    totals = {"ok": 0, "rate_limited": 0, "invalid": 0, "failed": 0}

    def publish(van: Van) -> str:
        try:
            client.send_telemetry(van.event())
            return "ok"
        except RateLimitedError:
            return "rate_limited"
        except ValidationError:
            return "invalid"
        except (GatewayError, OSError):
            return "failed"

    print(f"Simulating {len(vans)} vans around {center} -> http://{args.host}:{args.http_port}")
    started = time.monotonic()
    next_tick = started
    last_report = started
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        try:
            while args.duration <= 0 or time.monotonic() - started < args.duration:
                for van in vans:
                    van.step(args.interval, center, args.radius_km)
                online = [van for van in vans if van.state != "silent"]
                for outcome in pool.map(publish, online):
                    totals[outcome] += 1
                now = time.monotonic()
                if now - last_report >= 10:
                    silent = len(vans) - len(online)
                    print(f"[{now - started:6.0f}s] published={totals['ok']} "
                          f"rate_limited={totals['rate_limited']} invalid={totals['invalid']} "
                          f"failed={totals['failed']} silent_vans={silent}", flush=True)
                    last_report = now
                next_tick += args.interval
                time.sleep(max(0.0, next_tick - time.monotonic()))
        except KeyboardInterrupt:
            pass
    print(f"Done: {totals}")


if __name__ == "__main__":
    main()
