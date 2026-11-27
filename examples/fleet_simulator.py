#!/usr/bin/env python3
"""Simulate a delivery fleet publishing telemetry through the SDK.

Vans drive on the streets of a fictional demo city (dashboard/city_map.json,
the same map the dashboard draws). Each van belongs to a depot and works in
delivery rounds: it drives along the roads to 6-14 nearby addresses in turn,
stopping at each (idle), then returns to its depot to load up again. Driving
is modelled at 10 Hz: vans accelerate and brake smoothly,
slow down for corners, wait at traffic lights on the main roads and keep a
gap to the van in front. Now and then a van speeds on a main road (for the
alert demo) or loses signal for long enough to be marked offline, though it
keeps driving.

This is a demo/load *simulation*: every van publishes from this one
process, i.e. from one client IP. Run the gateway with config.demo.ini,
otherwise the per-IP rate limiter (correctly) rejects most of the traffic,
and the 429 count printed below shows it.

    python3 examples/fleet_simulator.py --vehicles 500

One process publishes up to roughly 1,500 reports/s (the Python SDK is the
limit), so keep --vehicles x (1 / --interval) below that.
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import random
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import _sdk_path  # noqa: F401
from gateway_client import GatewayClient, GatewayError, RateLimitedError, ValidationError

METRES_PER_DEGREE = 111_320.0
DEFAULT_MAP = Path(__file__).resolve().parent.parent / "dashboard" / "city_map.json"

PHYSICS_STEP = 0.1          # seconds per simulation step
ACCELERATION = 1.4          # m/s^2
COMFORT_BRAKE = 1.8         # m/s^2, used to plan stops and corners
MAX_BRAKE = 4.0             # m/s^2
FOLLOW_GAP = 9.0            # metres kept to the van ahead
STOP_LINE = 6.0             # metres before a junction to stop at a red light
LIGHT_CYCLE = 36.0          # seconds; half green for each axis
CRUISE_KMH = {"major": (34, 44), "minor": (22, 30)}
SPEEDING_KMH = (55, 65)
LANE_OFFSET = 3.5           # metres left of the centre line (UK: drive on the left)
DELIVERY_RADIUS = 1200.0    # metres; next address is usually this close
DELIVERIES_PER_ROUND = (6, 14)


def kmh(metres_per_second: float) -> float:
    return metres_per_second * 3.6


class City:
    """Road graph in local metres, built from the shared map file."""

    def __init__(self, path: Path) -> None:
        data = json.loads(path.read_text())
        bounds = data["bounds"]
        self.lat0 = (bounds["min_lat"] + bounds["max_lat"]) / 2
        self.lng0 = (bounds["min_lng"] + bounds["max_lng"]) / 2
        self.lng_scale = METRES_PER_DEGREE * math.cos(math.radians(self.lat0))
        self.points = [self.to_xy(lat, lng) for lat, lng in data["nodes"]]
        self.neighbours: dict[int, list[int]] = {i: [] for i in range(len(self.points))}
        self.road_class: dict[tuple, str] = {}
        major_degree = {i: 0 for i in range(len(self.points))}
        for road in data["roads"]:
            for a, b in zip(road["nodes"], road["nodes"][1:]):
                self.neighbours[a].append(b)
                self.neighbours[b].append(a)
                self.road_class[(a, b)] = self.road_class[(b, a)] = road["class"]
                if road["class"] == "major":
                    major_degree[a] += 1
                    major_degree[b] += 1
        # Traffic lights at crossroads on the main roads.
        # The value is each light's phase offset within the cycle.
        self.lights = {i: random.uniform(0, LIGHT_CYCLE) for i in range(len(self.points))
                       if major_degree[i] >= 2 and len(self.neighbours[i]) >= 4}
        self.depots = [depot["node"] for depot in data["depots"]]

    def distance(self, a: int, b: int) -> float:
        (ax, ay), (bx, by) = self.points[a], self.points[b]
        return math.hypot(bx - ax, by - ay)

    def node_near(self, node: int, radius: float) -> int:
        """A random junction within radius of node (or anywhere, rarely)."""
        for _ in range(40):
            candidate = random.randrange(len(self.points))
            if candidate != node and self.distance(node, candidate) <= radius:
                return candidate
        return random.randrange(len(self.points))

    def to_xy(self, lat: float, lng: float) -> tuple:
        return ((lng - self.lng0) * self.lng_scale, (lat - self.lat0) * METRES_PER_DEGREE)

    def to_lat_lng(self, x: float, y: float) -> tuple:
        return (self.lat0 + y / METRES_PER_DEGREE, self.lng0 + x / self.lng_scale)

    def length(self, a: int, b: int) -> float:
        (ax, ay), (bx, by) = self.points[a], self.points[b]
        return math.hypot(bx - ax, by - ay)

    def turn_angle(self, a: int, b: int, c: int) -> float:
        """Change of direction (radians, 0 = straight on) driving a -> b -> c."""
        (ax, ay), (bx, by), (cx, cy) = self.points[a], self.points[b], self.points[c]
        h1 = math.atan2(by - ay, bx - ax)
        h2 = math.atan2(cy - by, cx - bx)
        return abs((h2 - h1 + math.pi) % (2 * math.pi) - math.pi)

    def light_is_green(self, node: int, a: int, now: float) -> bool:
        """Green for east-west approaches in the first half of the cycle."""
        (ax, ay), (nx, ny) = self.points[a], self.points[node]
        east_west = abs(nx - ax) >= abs(ny - ay)
        first_half = (now + self.lights[node]) % LIGHT_CYCLE < LIGHT_CYCLE / 2
        return first_half == east_west

    def route(self, start: int, goal: int) -> list:
        """Quickest path by expected travel time (main roads are faster)."""
        best = {start: 0.0}
        previous = {}
        queue = [(0.0, start)]
        while queue:
            cost, node = heapq.heappop(queue)
            if node == goal:
                break
            if cost > best[node]:
                continue
            for nxt in self.neighbours[node]:
                speed = 11.0 if self.road_class[(node, nxt)] == "major" else 7.5
                new_cost = cost + self.length(node, nxt) / speed
                if new_cost < best.get(nxt, math.inf):
                    best[nxt] = new_cost
                    previous[nxt] = node
                    heapq.heappush(queue, (new_cost, nxt))
        path = [goal]
        while path[-1] != start:
            path.append(previous[path[-1]])
        return path[::-1]


class Van:
    def __init__(self, vehicle_id: str, city: City) -> None:
        self.vehicle_id = vehicle_id
        self.city = city
        self.speed = 0.0                    # m/s
        self.state = "moving"               # moving | idle
        self.idle_left = 0.0
        self.silent_left = 0.0
        self.speeding_left = 0.0
        self.style = random.uniform(0.0, 1.0)  # Where in the cruise range this driver sits.
        self.depot = random.choice(city.depots)
        # Start somewhere in this depot's area, part-way through a round.
        a = city.node_near(self.depot, 2 * DELIVERY_RADIUS)
        b = random.choice(city.neighbours[a])
        self.route = [a, b]
        self.leg = 0                        # Driving route[leg] -> route[leg + 1].
        self.pos = random.uniform(0, city.length(a, b))
        self.stop_at = city.length(a, b)
        self.deliveries_left = random.randint(1, DELIVERIES_PER_ROUND[1])
        self.plan_next_stop()

    # --- routing ---------------------------------------------------------

    @property
    def edge(self) -> tuple:
        return self.route[self.leg], self.route[self.leg + 1]

    def plan_next_stop(self) -> None:
        """Route to the next nearby address, or back to the depot."""
        city = self.city
        a, b = self.edge
        if self.deliveries_left > 0:
            goal_a = city.node_near(b, DELIVERY_RADIUS)
            goal_b = random.choice(city.neighbours[goal_a])
            stop_fraction = random.uniform(0.25, 0.75)
        else:
            # Pull into the depot: stop just short of its junction.
            goal_b = self.depot
            goal_a = random.choice(city.neighbours[goal_b])
            stop_fraction = max(0.0, 1.0 - 12.0 / city.length(goal_a, goal_b))
        path = city.route(b, goal_a)
        self.route = [a] + path + [goal_b]
        self.leg = 0
        # If the address is back along the street just driven, the route
        # turns round at the next junction, as a real van would.
        self.stop_at = stop_fraction * city.length(goal_a, goal_b)

    # --- driving ---------------------------------------------------------

    def target_speed(self, now: float, leaders: dict) -> float:
        city = self.city
        a, b = self.edge
        lo, hi = SPEEDING_KMH if self.speeding_left > 0 else CRUISE_KMH[city.road_class[(a, b)]]
        target = (lo + (hi - lo) * self.style) / 3.6
        remaining = city.length(a, b) - self.pos
        final_leg = self.leg == len(self.route) - 2

        def cap(stop_distance: float, speed_at_stop: float) -> None:
            nonlocal target
            allowed = math.sqrt(speed_at_stop ** 2 + 2 * COMFORT_BRAKE * max(stop_distance, 0.0))
            target = min(target, allowed)

        if final_leg:
            cap(self.stop_at - self.pos, 0.0)
        else:
            c = self.route[self.leg + 2]
            angle = city.turn_angle(a, b, c)
            if angle > 0.35:
                # ~20 km/h for a gentle bend down to ~8 km/h for a U-turn.
                cap(remaining, (20 - 12 * min(angle / math.pi, 1.0)) / 3.6)
            if b in city.lights and not city.light_is_green(b, a, now):
                if remaining > STOP_LINE - 1.0:
                    cap(remaining - STOP_LINE, 0.0)
        gap = leaders.get(self.vehicle_id)
        if gap is not None:
            cap(gap - FOLLOW_GAP, 0.0)
        return target

    def step(self, dt: float, now: float, leaders: dict) -> None:
        if self.silent_left > 0:
            self.silent_left -= dt
        if self.state == "idle":
            self.speed = 0.0
            self.idle_left -= dt
            if self.idle_left <= 0:
                self.state = "moving"
                self.plan_next_stop()
            return

        self.speeding_left = max(0.0, self.speeding_left - dt)
        target = self.target_speed(now, leaders)
        change = max(-MAX_BRAKE * dt, min(ACCELERATION * dt, target - self.speed))
        self.speed = max(0.0, self.speed + change)
        self.pos += self.speed * dt

        city = self.city
        while True:
            a, b = self.edge
            final_leg = self.leg == len(self.route) - 2
            if final_leg:
                if self.pos >= self.stop_at - 0.5 and self.speed < 0.6:
                    self.pos = min(self.pos, self.stop_at)
                    self.speed = 0.0
                    self.state = "idle"
                    if self.deliveries_left > 0:
                        self.deliveries_left -= 1
                        self.idle_left = random.uniform(12, 40)   # Handing over a parcel.
                    else:
                        self.deliveries_left = random.randint(*DELIVERIES_PER_ROUND)
                        self.idle_left = random.uniform(60, 180)  # Loading at the depot.
                break
            length = city.length(a, b)
            if self.pos < length:
                break
            self.pos -= length
            self.leg += 1

    def maybe_change_mood(self, interval: float) -> None:
        """Rare per-report events: speeding on a main road, losing signal."""
        if self.state != "moving":
            return
        a, b = self.edge
        if (self.speeding_left <= 0 and self.city.road_class[(a, b)] == "major"
                and random.random() < 0.006 * interval):
            self.speeding_left = random.uniform(8, 15)
        if self.silent_left <= 0 and random.random() < 0.002 * interval:
            self.silent_left = random.uniform(35, 70)

    def position(self) -> tuple:
        a, b = self.edge
        (ax, ay), (bx, by) = self.city.points[a], self.city.points[b]
        length = max(self.city.length(a, b), 1e-6)
        f = min(max(self.pos / length, 0.0), 1.0)
        # Report the van in its lane, as a real GPS would, so traffic in
        # opposite directions is drawn side by side when zoomed in.
        left_x, left_y = -(by - ay) / length, (bx - ax) / length
        return self.city.to_lat_lng(ax + (bx - ax) * f + left_x * LANE_OFFSET,
                                    ay + (by - ay) * f + left_y * LANE_OFFSET)

    def event(self) -> dict:
        lat, lng = self.position()
        return {
            "vehicle_id": self.vehicle_id,
            "lat": round(lat, 6),
            "lng": round(lng, 6),
            "speed": round(kmh(self.speed), 1),
            "status": "idle" if self.state == "idle" else "moving",
            "timestamp": int(time.time()),
        }


def gaps_to_leaders(vans: list) -> dict:
    """Distance from each van to the next van ahead on the same street."""
    by_edge: dict[tuple, list] = {}
    for van in vans:
        by_edge.setdefault(van.edge, []).append(van)
    gaps = {}
    for group in by_edge.values():
        if len(group) < 2:
            continue
        group.sort(key=lambda v: v.pos)
        for behind, ahead in zip(group, group[1:]):
            gaps[behind.vehicle_id] = ahead.pos - behind.pos
    return gaps


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--vehicles", type=int, default=25)
    parser.add_argument("--interval", type=float, default=1.0, help="seconds between reports")
    parser.add_argument("--duration", type=float, default=0, help="stop after N seconds (0 = run forever)")
    parser.add_argument("--map", type=Path, default=DEFAULT_MAP, help="road map JSON")
    parser.add_argument("--workers", type=int, default=8, help="concurrent HTTP requests")
    parser.add_argument("--seed", type=int, default=None)
    args = parser.parse_args()

    random.seed(args.seed)
    city = City(args.map)
    vans = [Van(f"van-{index + 1:05d}", city) for index in range(args.vehicles)]

    client = GatewayClient(args.host, args.http_port)
    totals = {"ok": 0, "rate_limited": 0, "invalid": 0, "failed": 0}

    def publish(event: dict) -> str:
        try:
            client.send_telemetry(event)
            return "ok"
        except RateLimitedError:
            return "rate_limited"
        except ValidationError:
            return "invalid"
        except (GatewayError, OSError):
            return "failed"

    print(f"Simulating {len(vans)} vans from {len(city.depots)} depots in {args.map.name} "
          f"({len(city.points)} junctions, {len(city.lights)} traffic lights) "
          f"-> http://{args.host}:{args.http_port}")
    steps_per_report = max(1, round(args.interval / PHYSICS_STEP))
    dt = args.interval / steps_per_report
    started = time.monotonic()
    sim_time = 0.0
    next_tick = started
    last_report = started
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        try:
            while args.duration <= 0 or time.monotonic() - started < args.duration:
                for _ in range(steps_per_report):
                    leaders = gaps_to_leaders(vans)
                    for van in vans:
                        van.step(dt, sim_time, leaders)
                    sim_time += dt
                for van in vans:
                    van.maybe_change_mood(args.interval)
                # Snapshot every position at the same instant, then publish.
                online = [van.event() for van in vans if van.silent_left <= 0]
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
