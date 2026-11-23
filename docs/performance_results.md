# Performance Results

Sustained-throughput measurements of the publish path (`POST /publish`:
parse → rate-limit check → validate → log append → fleet update → response).
They complement the burst-capacity tests in [results.md](results.md), which
open up to 50,000 connections and send one request each.

**Headline:** on one laptop, the gateway sustains **~77,000 validated
telemetry events/s at 1.03 ms P99** (120-byte events, 100 connections), and
**~35,000 events/s with 8 KiB bodies (~280 MB/s into the log)**. It had zero
errors in 54 benchmark runs. This is simulated load from `wrk` on the same
machine, not real vehicles.

## 1. Setup

| Item | Value |
| --- | --- |
| CPU | Intel Core i5-1240P (4 P-cores + 8 E-cores, 16 threads), laptop, frequency scaling on |
| OS | Ubuntu, Linux 7.0 |
| Build | CMake `Release` (`-O3`), GCC |
| Gateway | 4 epoll workers, pool threads varied, queue 4096, rate limit effectively off, 256 MiB segments, fresh empty log per run |
| Placement | gateway pinned to CPUs 0–7 (P-cores), `wrk` to CPUs 8–15 (E-cores) with `taskset` |
| Load | `wrk -t4`, 2 s warm-up, then 8 s measured, **3 repeats per cell, median reported** |
| Request | valid TelemetryEvent JSON (`tests/benchmarks/telemetry.lua`); 1 KiB and 8 KiB bodies padded with an ignored `note` field |
| Connections | one request per connection (the gateway sends `Connection: close`), so every request includes a TCP handshake |

Reproduce with `tests/benchmarks/run_bench.sh`. Raw data:
[`tests/benchmarks/results/matrix-2026-11-24.csv`](../tests/benchmarks/results/matrix-2026-11-24.csv).

## 2. Results

| Pool threads | Body | Connections | Req/s (median) | P50 ms | P90 ms | P99 ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 120 B | 100 | 77,040 | 0.28 | 0.70 | 1.03 |
| 2 | 120 B | 1000 | 69,985 | 2.66 | 4.84 | 8.65 |
| 2 | 1 KiB | 100 | 66,555 | 0.33 | 0.91 | 98.62 |
| 2 | 1 KiB | 1000 | 62,551 | 2.89 | 5.34 | 110.04 |
| 2 | 8 KiB | 100 | 35,136 | 2.44 | 3.46 | 98.16 |
| 2 | 8 KiB | 1000 | 34,712 | 25.34 | 40.38 | 131.57 |
| 4 | 120 B | 100 | 70,107 | 0.32 | 0.78 | 1.16 |
| 4 | 120 B | 1000 | 65,299 | 2.99 | 5.36 | 8.95 |
| 4 | 1 KiB | 100 | 64,705 | 0.34 | 0.94 | 101.13 |
| 4 | 1 KiB | 1000 | 60,700 | 2.96 | 5.30 | 123.75 |
| 4 | 8 KiB | 100 | 34,602 | 2.45 | 3.96 | 99.49 |
| 4 | 8 KiB | 1000 | 34,732 | 25.36 | 39.68 | 137.88 |
| 8 | 120 B | 100 | 68,903 | 0.31 | 0.78 | 1.28 |
| 8 | 120 B | 1000 | 63,983 | 2.96 | 5.43 | 8.99 |
| 8 | 1 KiB | 100 | 64,240 | 0.35 | 0.98 | 125.12 |
| 8 | 1 KiB | 1000 | 59,765 | 3.20 | 6.20 | 115.71 |
| 8 | 8 KiB | 100 | 33,364 | 2.49 | 15.90 | 118.48 |
| 8 | 8 KiB | 1000 | 31,521 | 26.28 | 96.23 | 627.31 |

All 54 runs: 0 non-2xx responses, 0 socket errors.

## 3. Interpretation

**More pool threads did not help, and slightly hurt.** Throughput is flat or
falling from 2 to 8 pool threads in every row. Each publish holds the single
log mutex for the append, and the work outside it (validation, fleet update)
is short. Extra threads therefore mostly queue on that mutex and compete with
the 4 epoll workers for the 8 P-core threads. At 8 threads with 8 KiB bodies
and 1000 connections, P90 rises from about 40 ms to 96 ms. That cell also had
the widest spread between repeats (P99 627 ms median), which fits CPU
oversubscription rather than a steady cost. The best setting on this machine
is 2 pool threads.

**Connections affect latency, not throughput.** Going from 100 to 1000
connections raises P50 about tenfold (0.28 → 2.66 ms) while requests/s drop
only about 9%. The extra clients just queue. This is Little's law: at a fixed
completion rate, ten times the outstanding requests means about ten times
the time each one waits.

**Body size: throughput in bytes keeps rising.** 120 B → 8 KiB is 68× more
data per request for about 2.2× fewer requests, so about 9 MB/s becomes
about 285 MB/s. Per-request overhead (TCP handshake, HTTP parse,
scheduling) dominates small events; per-byte work (CRC, JSON scan,
`memcpy`) dominates large ones.

**The P99 cliff at 1 KiB and above is segment rollover.** With 120-byte
events, P99 tracks P90 (about 1 ms). From 1 KiB up, P99 jumps to about 100 ms
while P50/P90 stay low. The cause: when a 256 MiB segment fills,
`append()` runs a synchronous `msync` + `fsync` of the whole segment
**while holding the log mutex**, so every publish waits behind it. At
65–280 MB/s a segment fills every 1–4 s, inside the 8 s window. A 120-byte
run writes only about 70 MB and never rolls over. The hypothesis was tested
directly with the same 1 KiB / 4 threads / 100 connections cell (2 repeats
each):

| Segment size | Rollovers in the run | Req/s | P50 ms | P99 ms |
| ---: | --- | ---: | ---: | ---: |
| 64 MiB | ~10 | 69,477 | 0.33 | 28.0 |
| 256 MiB (matrix default) | 2–3 | 64,705 | 0.34 | 101.1 |
| 1 GiB | 0 | 70,112 | 0.33 | 1.19 |

With no rollover the tail disappears. Smaller segments mean shorter but more
frequent stalls. The fix, not implemented, is to hand the sealed segment to
a background thread for `msync`/`fsync`/`munmap`, so appends to the new
segment never wait for it. This is listed as future work. Raw data:
`tests/benchmarks/results/rollover-segment-*.csv`.

## 4. The optimisation that was made

The first runs showed 8 KiB bodies at only about 10.5k req/s. `perf` needs
root on this machine (`perf_event_paranoid = 4`), so the gateway was profiled
with `valgrind --tool=callgrind` under an 8 KiB load:

| Function | Instructions |
| --- | ---: |
| `MemoryMappedLog::append` (bit-at-a-time CRC32, inside the log mutex) | 61.2% |
| `FlatJsonReader::read_string` (one `+=` per character) | 30.9% |

After a compile-time CRC table (bit-identical output, enforced by a golden
test) and run-based string copying, measured with 4 pool threads and 100
connections:

| Body | Before | After |
| ---: | ---: | ---: |
| 120 B | 69.5k req/s | 74.6k req/s |
| 8 KiB | 10.5k req/s | 33.1k req/s (**3.2×**) |

The full story is in [journal.md](journal.md). To profile with `perf`
instead (needs root):

```bash
sudo perf record -g -p "$(pgrep -n message_gateway)" -- sleep 10   # while wrk runs
sudo perf report --stdio --no-children | head -40
```

## 5. Cache vs. log reads

`./build/cache_vs_disk_bench` (Release) times returning the newest 100
records from a 200,000-record log:

| Path | Time per call |
| --- | ---: |
| `get_recent(100)`: in-memory ring buffer (`/recent`) | 2.1 µs |
| `read_from(offset)`: one batched `pread` + CRC per record (consumer replay) | 68.7 µs |
| 100 × `read_from(…, 1 record)` from a sealed segment | 154.7 µs |

The cache is about 33× faster than a batched log read, even though the log
files are hot in the page cache. These numbers measure syscalls, copies and
checksums, not physical disk latency. This is why `/recent` and the fleet
rebuild use the ring buffer while consumers use the log, which is the source
of truth.

## 6. What these numbers do not show

- **Same-host loopback.** Client and server share one machine and memory
  bus, with no real network latency. Absolute numbers will differ on servers
  and across a network.
- **Laptop CPU.** Frequency scaling and the P/E-core split add noise: repeats
  of one cell differ by a median of 2% in throughput (worst cell 9%) and much more in P99.
- **No keep-alive.** Every request pays for a TCP handshake. A keep-alive
  client would show higher throughput, which reflects the current gateway
  honestly but differs from typical HTTP benchmarks.
- **Rate limiting disabled** for the benchmark. With the default policy a
  single IP is limited to 5 requests/s by design.
- **Durability level.** Throughput is measured with page-cache durability
  (survives `kill -9`), not per-record `fsync`.
