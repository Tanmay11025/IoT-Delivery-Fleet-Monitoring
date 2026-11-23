// Compares the two ways the gateway can return "the last N records":
//   get_recent(N): copy out of the in-memory CircularBuffer (what /recent uses)
//   read_from(o):  pread + header/CRC checks from the segment files (what a
//                  consumer replay uses)
//
// The files were just written, so read_from hits the kernel page cache: this
// measures syscall + copy + checksum cost, not physical disk latency.
#include "storage/mmap_log.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
template <typename Function>
double nanoseconds_per_call(int iterations, Function function) {
    const auto start = std::chrono::steady_clock::now();
    std::size_t sink = 0;
    for (int index = 0; index < iterations; ++index) sink += function();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (sink == 0) std::puts("");  // Keep the calls from being optimised away.
    return std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("gateway-cache-bench-" + std::to_string(getpid()));
    std::filesystem::remove_all(directory);
    constexpr int records = 200000;
    constexpr int window = 100;
    constexpr int iterations = 20000;

    MemoryMappedLog log;
    log.open(directory, 16 * 1024 * 1024, 1024);
    for (int index = 0; index < records; ++index) {
        log.append("{\"vehicle_id\":\"van-" + std::to_string(index % 2000) +
                   "\",\"lat\":51.5074,\"lng\":-0.1278,\"speed\":32.5,"
                   "\"status\":\"moving\",\"timestamp\":1726310400}");
    }
    const std::uint64_t tail_offset = log.get_recent(window).front().offset;

    const double cache_ns = nanoseconds_per_call(iterations, [&] {
        return log.get_recent(window).size();
    });
    const double tail_ns = nanoseconds_per_call(iterations, [&] {
        return log.read_from(tail_offset, 1 << 20).messages.size();
    });
    // Worst case: the oldest records (a sealed segment) fetched one call per
    // record, which reopens the segment file every time.
    const double sealed_ns = nanoseconds_per_call(iterations / 10, [&] {
        auto result = log.read_from(0, 1);
        std::size_t total = result.messages.size();
        for (int index = 1; index < window; ++index) {
            result = log.read_from(result.next_offset, 1);
            total += result.messages.size();
        }
        return total;
    });

    std::printf("records in log: %d (%zu segments), window: last %d records\n", records,
                log.segments().size(), window);
    std::printf("%-44s %10.1f us/call\n", "get_recent(100)      [in-memory cache]", cache_ns / 1000);
    std::printf("%-44s %10.1f us/call\n", "read_from(tail, 1 MiB) [one pread batch]", tail_ns / 1000);
    std::printf("%-44s %10.1f us/call\n", "read_from x100, 1 record each [sealed]", sealed_ns / 1000);
    std::printf("cache speed-up vs batched read: %.1fx\n", tail_ns / cache_ns);
    log.close();
    std::filesystem::remove_all(directory);
    return 0;
}
