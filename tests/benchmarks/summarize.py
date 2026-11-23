#!/usr/bin/env python3
"""Summarise a run_bench.sh CSV as a Markdown table (median of the repeats)."""

import csv
import statistics
import sys
from collections import defaultdict


def main(path: str) -> None:
    cells = defaultdict(list)
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            key = (int(row["pool_threads"]), int(row["payload_bytes"]), int(row["connections"]))
            cells[key].append(row)

    print("| Pool threads | Body | Connections | Req/s (median) | P50 ms | P90 ms | P99 ms "
          "| Non-2xx | Socket errors | Runs |")
    print("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for (pool, payload, connections), rows in sorted(cells.items()):
        def median(field):
            return statistics.median(float(row[field]) for row in rows)
        body = f"{payload} B" if payload < 1024 else f"{payload // 1024} KiB"
        print(f"| {pool} | {body} | {connections} | {median('rps'):,.0f} | "
              f"{median('p50_ms'):.2f} | {median('p90_ms'):.2f} | {median('p99_ms'):.2f} | "
              f"{sum(int(row['non_2xx']) for row in rows)} | "
              f"{sum(int(row['socket_errors']) for row in rows)} | {len(rows)} |")


if __name__ == "__main__":
    main(sys.argv[1])
