# Engineering Journal

Problems that took real diagnosis, written down while they were fresh. Each
entry covers what happened, how the cause was found, and what changed.
Numbers come from the runs described; nothing here is reconstructed from
memory.

---

## 2026-10 — The rate limiter broke the integration suite

**Symptom.** After per-IP token-bucket rate limiting was added, the Docker
HTTP integration suite started failing part-way through with
`429 Too Many Requests`. The order of failures depended on test order.

**Diagnosis.** Every test client ran in one container, so the gateway saw one
source IP. The earlier tests used up the default 10-token bucket and the later
ones were throttled. The limiter was behaving correctly; the tests had
quietly assumed there was no limit.

**Fix.** Binding clients to different loopback addresses was not portable
inside the Alpine network namespace. Instead, the integration Compose service
starts the gateway with a large capacity and zero refill
(`docker/docker-compose.http-test.yml`). The production default stays strict,
and transport tests no longer depend on rate-limit policy.
(Also recorded in results.md §3.2.)

**Lesson reused later.** The same "every client shares one IP" problem was
predicted for the fleet simulator before it was written. That is why
`config.demo.ini` exists and why the simulator prints its 429 count.

---

## 2026-11-24 — A consumer could stall with records still unsent

**Symptom.** Found while reviewing the first version of
`TCPServer::pump_consumer`, before any test exercised it. A consumer could stop
receiving data while the log still had records for it.

**Diagnosis.** The pump filled the consumer's queue up to the 256 KiB cap
and then called `flush_client`. If the kernel accepted everything in one go,
`flush_client` returned with nothing pending and without registering
`EPOLLOUT`. The `data_available` flag had already been cleared, so no event
would ever call the pump again until an unrelated publish arrived. During a
large replay with no new traffic, the consumer would wait forever.

**Fix.** The pump now loops: fill to the cap, flush, and if the socket
drained completely, fill again. It returns only when caught up with the log
or when the kernel buffer is full, and in the second case `EPOLLOUT` is
guaranteed to resume it. Regression test:
`test_replay_larger_than_the_send_queue_completes`, which replays about
2 MB of history, many times the per-consumer cap, with no new publishes
during the replay.

---

## 2026-11-24 — "The gateway doesn't shut down under valgrind"

**Symptom.** While profiling, the gateway ran under callgrind, `wrk`
finished, and `kill -INT $VG; wait $VG` hung for minutes. Natively, SIGINT
shutdown had worked in every test.

**Diagnosis.** `callgrind_control -b <pid>` printed live backtraces. All four
pool threads were idle in `condition_variable::wait`, the main thread was in
`thread::join`, and both epoll workers were cycling through `epoll_wait`
every 100 ms (2,319 calls so far). The loops were healthy but had never seen
the shutdown flag, so the signal had never arrived. The cause was the shell
command: `cd dir && valgrind ... &` puts the whole `&&` chain in the
background, so `$!` was the PID of a subshell, not of valgrind.

**Fix.** Sending SIGINT to the real PID shut the gateway down in about 400 ms.
No code change was needed. The useful habit was to collect thread
backtraces before assuming a deadlock.

---

## 2026-11-24 — 8 KiB telemetry ran at a third of the expected rate

**Symptom.** In the first `wrk` runs (4 pool threads, 100 connections),
120-byte events reached about 69.5k req/s, but 8 KiB events reached only
about 10.5k req/s, roughly 86 MB/s. That is far below what `memcpy` into a
mapped file should allow.

**Hypothesis and measurement.** If copying were the cost, throughput in
bytes would stay roughly flat as bodies grew. It fell instead, which
pointed to per-byte CPU work. `perf` was not available
(`perf_event_paranoid = 4`), so the gateway ran under
`valgrind --tool=callgrind` with an 8 KiB load. Results:

| Function | Share of instructions |
| --- | ---: |
| `MemoryMappedLog::append` (bit-at-a-time CRC32 inlined) | 61.2% |
| `FlatJsonReader::read_string` (per-character `+=`) | 30.9% |
| everything else | < 8% |

The CRC loop ran eight iterations per byte, *inside the log mutex*, so it
serialised every publish across all pool threads.

**Fix.** A compile-time 256-entry CRC table (one lookup per byte), plus
copying runs of plain characters in the JSON string reader with one
`append`. The CRC must stay bit-identical, or existing segments would fail
recovery. So before the change, a new test
(`mmap_log_writes_standard_crc32_checksums`) was added that checks on-disk
CRCs against a reference implementation and the standard `"123456789"` →
`0xCBF43926` vector. It passed before and after the change.

**Result (same machine and settings).** 8 KiB: 10.5k → 33.1k req/s.
120 B: 69.5k → 74.6k req/s. Reading 100 records back with `read_from`:
155 µs → 69 µs.

---

## 2026-11-24 — Builds without a build type were unoptimised

**Symptom.** While setting up the benchmarks: `CMakeLists.txt` never set
`CMAKE_BUILD_TYPE`, and the Dockerfile ran a plain `cmake -S . -B build`.

**Why it matters.** With an empty build type, CMake adds no `-O` flags at
all, so the Docker image (and therefore the staged 100–50,000 client runs in
results.md) used an unoptimised binary. Those results are still valid as
correctness and capacity evidence, but their latencies are pessimistic.

**Fix.** CMake now defaults to `Release` when no build type is given. The
benchmarks in performance_results.md were all built in Release. CI's
sanitizer job sets `Debug` explicitly.

---

## 2026-11-24 — 100 ms P99 spikes once bodies reach 1 KiB

**Symptom.** In the full benchmark matrix, 120-byte events had P99 ≈ P90
(about 1 ms). From 1 KiB up, P99 jumped to about 100 ms while P50 and P90
barely moved. Throughput was fine.

**Hypothesis.** A rare, long stall, not general slowness. Candidate: segment
rollover. When a segment fills, `append()` `msync`s and `fsync`s the whole
256 MiB file while holding the log mutex, so every publish queues behind it.
At 1 KiB × 65k/s a segment fills every ~4 s, inside the 8 s run. The 120-byte
runs never fill one.

**Test.** The same cell was re-run with only the segment size changed:
64 MiB → P99 28 ms, 256 MiB → 101 ms, 1 GiB (no rollover) → 1.19 ms.
Throughput and P50 were unchanged in all three. The tail is entirely
rollover, and its size scales with how much there is to sync.

**Status.** Documented, not fixed. The fix is to move `msync`/`fsync`/`munmap`
of the sealed segment to a background thread, so appends to the new segment
never wait. It needs care so that recovery still sees a fully synced segment
before it is treated as sealed. Details: performance_results.md §3.
