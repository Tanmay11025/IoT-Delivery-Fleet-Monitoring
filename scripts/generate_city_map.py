#!/usr/bin/env python3
"""Generate dashboard/city_map.json: the demo city the vans drive in.

The city is fictional: about 6 x 3.4 km of irregular street grid in nine
named neighbourhoods, split by a winding river with embankment roads on both
banks and a handful of bridges, plus main roads every few blocks, two
diagonal avenues, parks and four delivery depots. It is anchored at a real
latitude/longitude so the telemetry stays valid. The same file is read by
examples/fleet_simulator.py (to route vans along the roads) and by the
dashboard (to draw the streets under the vans).

The output is deterministic, so re-running this script reproduces the
committed file byte for byte:

    python3 scripts/generate_city_map.py
"""

from __future__ import annotations

import json
import math
import random
from collections import deque
from pathlib import Path

ORIGIN = (51.5074, -0.1278)  # Central London; the city itself is made up.
METRES_PER_DEGREE = 111_320.0
WIDTH, HEIGHT = 6000.0, 3400.0
BLOCK = (200.0, 270.0)        # spacing between parallel streets, metres
RIVER_HALF_WIDTH = 55.0
EMBANKMENT_OFFSET = 85.0      # embankment road centre line from river centre
RIVER_CLEARANCE = 155.0       # no grid junction closer than this to the river
MAJOR_EVERY = 5               # every 5th column and 4th row is a main road
BRIDGE_COLUMNS = {0, 3}       # column index mod MAJOR_EVERY that gets a bridge
MINOR_REMOVAL_SHARE = 0.05

NEIGHBOURHOODS = [
    ["Westbury", "Northfield", "Eastwick"],
    ["Kingsbridge", "Old Town", "Harbour Quarter"],
    ["Millbrook", "Southmere", "Ashford Vale"],
]
NAME_STEMS = [
    "Albion", "Ash", "Aspen", "Baker", "Beacon", "Birch", "Bramble", "Brook",
    "Canal", "Cedar", "Chapel", "Cherry", "Church", "Clover", "Copper",
    "Crown", "Dale", "Elm", "Fairview", "Fern", "Forge", "Foundry", "Garden",
    "George", "Glebe", "Granary", "Hatton", "Hawthorn", "Hazel", "Heath",
    "Holly", "Ivy", "Juniper", "King's", "Lark", "Laurel", "Linden", "Maple",
    "Market", "Meadow", "Mill", "Mint", "Oak", "Orchard", "Park", "Pine",
    "Poplar", "Priory", "Queen's", "Railway", "Rope", "Rose", "Rowan",
    "Saxon", "School", "Silver", "Spring", "Station", "Tanner", "Thistle",
    "Tower", "Victoria", "Walnut", "Weaver", "Well", "Wharf", "Willow",
    "Windmill", "Yew", "Abbey", "Bell", "Bridge", "Castle", "Dock", "Ferry",
    "Granite", "Harbour", "Lantern", "Mason", "Northgate", "Pottery", "Quarry",
    "Regent", "Salt", "Sheaf", "Smith", "Southgate", "Temple", "Union",
    "Vine", "Water", "Westgate", "Wool", "Anchor", "Barley", "Cotton",
    "Draper", "Falcon", "Garnet", "Harvest", "Iron", "Kiln", "Lime", "Mulberry",
]
MINOR_SUFFIXES = ["Street", "Lane", "Road", "Row", "Grove", "Place", "Walk", "Terrace"]
MAJOR_SUFFIXES = ["Road", "Avenue", "Way", "Parade", "High Road", "Broadway"]


def river_y(x: float) -> float:
    return -150 + 260 * math.sin(x / 1100 + 0.4) + 90 * math.sin(x / 430)


def to_lat_lng(x: float, y: float) -> list:
    lat = ORIGIN[0] + y / METRES_PER_DEGREE
    lng = ORIGIN[1] + x / (METRES_PER_DEGREE * math.cos(math.radians(ORIGIN[0])))
    return [round(lat, 6), round(lng, 6)]


def spaced(rng: random.Random, start: float, end: float) -> list:
    values = [start]
    while values[-1] + BLOCK[1] < end - BLOCK[0] / 2:
        values.append(values[-1] + rng.uniform(*BLOCK))
    values.append(end)
    return values


def main() -> None:
    rng = random.Random(11)
    xs = spaced(rng, -WIDTH / 2, WIDTH / 2)
    ys = spaced(rng, -HEIGHT / 2, HEIGHT / 2)
    last_c, last_r = len(xs) - 1, len(ys) - 1

    # --- junctions ---------------------------------------------------------
    pos: dict = {}
    for c, x in enumerate(xs):
        for r, y in enumerate(ys):
            if abs(y - river_y(x)) < RIVER_CLEARANCE:
                continue
            jx = rng.uniform(-22, 22) if 0 < c < last_c else 0.0
            jy = rng.uniform(-22, 22) if 0 < r < last_r else 0.0
            pos[(c, r)] = (x + jx, y + jy)
    for c, x in enumerate(xs):
        pos[("S", c)] = (x, river_y(x) - EMBANKMENT_OFFSET)
        pos[("N", c)] = (x, river_y(x) + EMBANKMENT_OFFSET)

    def north_of_river(key) -> bool:
        x, y = pos[key]
        return y > river_y(x)

    def is_major_column(c: int) -> bool:
        return c % MAJOR_EVERY == 0 or c == last_c

    def is_major_row(r: int) -> bool:
        return r % 4 == 0 or r == last_r

    # --- streets as runs of junctions -------------------------------------
    # Each run is (class, [keys]) and becomes one named road.
    runs: list = []
    edges: set = set()

    def add_run(cls: str, keys: list) -> None:
        if len(keys) < 2:
            return
        runs.append([cls, keys])
        for a, b in zip(keys, keys[1:]):
            edges.add(frozenset((a, b)))

    for r in range(len(ys)):
        current = []
        for c in range(len(xs)):
            if (c, r) in pos:
                current.append((c, r))
            else:
                add_run("major" if is_major_row(r) else "minor", current)
                current = []
        add_run("major" if is_major_row(r) else "minor", current)

    for c in range(len(xs)):
        south = [(c, r) for r in range(len(ys)) if (c, r) in pos and not north_of_river((c, r))]
        north = [(c, r) for r in range(len(ys)) if (c, r) in pos and north_of_river((c, r))]
        cls = "major" if is_major_column(c) else "minor"
        if c % MAJOR_EVERY in BRIDGE_COLUMNS:
            add_run("major" if cls == "major" else "minor", south + [("S", c), ("N", c)] + north)
        else:
            add_run(cls, south + [("S", c)])
            add_run(cls, [("N", c)] + north)

    add_run("major", [("S", c) for c in range(len(xs))])
    add_run("major", [("N", c) for c in range(len(xs))])

    # Two diagonal avenues cutting across the grid toward the centre.
    def diagonal(c, r, dc, dr):
        keys = []
        while (c, r) in pos and (not keys or north_of_river((c, r)) == north_of_river(keys[0])):
            keys.append((c, r))
            c, r = c + dc, r + dr
        return keys[:9]
    for start, step in (((1, last_r - 1), (1, -1)), ((last_c - 1, 1), (-1, 1))):
        keys = diagonal(*start, *step)
        runs.append(["major", keys, "diagonal"])
        for a, b in zip(keys, keys[1:]):
            edges.add(frozenset((a, b)))
    diagonal_keys = {k for run in runs if len(run) == 3 for k in run[1]}

    # --- parks ---------------------------------------------------------------
    def block_ok(c, r, size):
        cells = [(c + i, r + j) for i in range(size + 1) for j in range(size + 1)]
        if not all(cell in pos for cell in cells):
            return False
        if any(cell in diagonal_keys for cell in cells):
            return False
        sides = {north_of_river(cell) for cell in cells}
        return len(sides) == 1

    parks = []
    used = set()
    candidates = [(c, r) for c in range(1, last_c - 2) for r in range(1, last_r - 2)]
    rng.shuffle(candidates)
    for c, r in candidates:
        if len(parks) >= 10:
            break
        size = 2 if len(parks) < 4 else 1
        cells = {(c + i, r + j) for i in range(-1, size + 2) for j in range(-1, size + 2)}
        if cells & used or not block_ok(c, r, size):
            continue
        used |= cells
        parks.append((c, r, size))
        if size == 2:
            # A two-by-two park swallows the junction in its middle.
            centre = (c + 1, r + 1)
            for edge in [e for e in edges if centre in e]:
                edges.discard(edge)

    # --- thin out a few minor links, keeping the network connected ----------
    def connected(edge_set) -> bool:
        adjacency: dict = {}
        for edge in edge_set:
            a, b = tuple(edge)
            adjacency.setdefault(a, []).append(b)
            adjacency.setdefault(b, []).append(a)
        start = next(iter(adjacency))
        seen = {start}
        queue = deque([start])
        while queue:
            for nxt in adjacency[queue.popleft()]:
                if nxt not in seen:
                    seen.add(nxt)
                    queue.append(nxt)
        return len(seen) == len(adjacency)

    minor_edges = sorted((edge for run in runs if run[0] == "minor"
                          for edge in map(frozenset, zip(run[1], run[1][1:]))
                          if edge in edges), key=lambda e: sorted(map(str, e)))
    rng.shuffle(minor_edges)
    for edge in minor_edges[: int(len(minor_edges) * MINOR_REMOVAL_SHARE)]:
        edges.discard(edge)
        if not connected(edges):
            edges.add(edge)

    # --- assemble ------------------------------------------------------------
    used_keys = sorted({k for edge in edges for k in edge}, key=str)
    index = {key: i for i, key in enumerate(used_keys)}
    nodes = [to_lat_lng(*pos[key]) for key in used_keys]

    stems = NAME_STEMS[:]
    rng.shuffle(stems)
    names_used = set()

    def next_name(cls: str) -> str:
        suffixes = MAJOR_SUFFIXES if cls == "major" else MINOR_SUFFIXES
        while True:
            name = f"{stems[len(names_used) % len(stems)]} {rng.choice(suffixes)}"
            if name not in names_used:
                names_used.add(name)
                return name

    roads = []
    for run in runs:
        cls, keys = run[0], run[1]
        name = next_name(cls)
        if len(run) == 3:
            name = "Station Avenue" if keys[0][0] < last_c / 2 else "Harbour Way"
        elif keys[0][0] in ("S", "N"):
            name = ("South" if keys[0][0] == "S" else "North") + " Embankment"
        piece = [keys[0]]
        for a, b in zip(keys, keys[1:]):
            if frozenset((a, b)) in edges:
                piece.append(b)
            else:
                if len(piece) > 1:
                    roads.append({"name": name, "class": cls, "nodes": [index[k] for k in piece]})
                piece = [b]
        if len(piece) > 1:
            roads.append({"name": name, "class": cls, "nodes": [index[k] for k in piece]})

    river_top = [to_lat_lng(x, river_y(x) + RIVER_HALF_WIDTH)
                 for x in range(int(-WIDTH / 2 - 200), int(WIDTH / 2 + 201), 50)]
    river_bottom = [to_lat_lng(x, river_y(x) - RIVER_HALF_WIDTH)
                    for x in range(int(WIDTH / 2 + 200), int(-WIDTH / 2 - 201), -50)]
    areas = [{"kind": "water", "name": "River Fleet", "points": river_top + river_bottom}]

    park_names = ["Victoria Gardens", "Elm Green", "Kings Park", "Millbrook Common",
                  "Orchard Fields", "Beacon Hill Park", "Rose Gardens", "Abbey Green",
                  "Quarry Park", "Linden Square"]
    for (c, r, size), name in zip(parks, park_names):
        corners = [pos[(c, r)], pos[(c + size, r)], pos[(c + size, r + size)], pos[(c, r + size)]]
        cx = sum(p[0] for p in corners) / 4
        cy = sum(p[1] for p in corners) / 4
        inset = []
        for x, y in corners:
            dx, dy = cx - x, cy - y
            length = math.hypot(dx, dy)
            inset.append(to_lat_lng(x + dx / length * 20, y + dy / length * 20))
        areas.append({"kind": "park", "name": name, "points": inset})

    def nearest_node(x, y):
        return min(used_keys, key=lambda k: (pos[k][0] - x) ** 2 + (pos[k][1] - y) ** 2
                   if isinstance(k[0], int) else math.inf)

    depots = []
    for name, (x, y) in [("West depot", (-2000, 900)), ("North depot", (600, 1200)),
                         ("East depot", (2100, -1000)), ("South depot", (-700, -1250))]:
        depots.append({"name": name, "node": index[nearest_node(x, y)]})

    districts = []
    for row, names in enumerate(NEIGHBOURHOODS):
        for col, name in enumerate(names):
            x = (col - 1) * WIDTH / 3
            y = {0: 1150.0, 1: river_y(x) + 420, 2: -1250.0}[row]
            districts.append({"name": name, "at": to_lat_lng(x, y)})

    lats = [p[0] for p in nodes]
    lngs = [p[1] for p in nodes]
    city = {
        "name": "Fleetham (fictional demo city)",
        "bounds": {"min_lat": min(lats), "max_lat": max(lats),
                   "min_lng": min(lngs), "max_lng": max(lngs)},
        "depots": depots,
        "districts": districts,
        "nodes": nodes,
        "roads": roads,
        "areas": areas,
    }
    out = Path(__file__).resolve().parent.parent / "dashboard" / "city_map.json"
    out.write_text(json.dumps(city, separators=(",", ":")) + "\n")
    km = sum(math.dist(pos[a], pos[b]) for a, b in map(tuple, edges)) / 1000
    print(f"wrote {out}: {len(nodes)} junctions, {len(edges)} street segments, "
          f"{km:.0f} km of street, {len(parks)} parks")


if __name__ == "__main__":
    main()
