#pragma once

#include "telemetry_event.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Latest known state of every vehicle, kept in memory for the dashboard.
//
// The log is the source of truth; FleetState is a derived view that can be
// rebuilt from it. Updates come from request-pool threads, reads from
// /vehicles and /metrics, so a shared_mutex lets reads run concurrently.
class FleetState {
public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    struct Vehicle {
        TelemetryEvent event;          // Last accepted report.
        std::uint64_t offset = 0;      // Log offset of that report.
        double age_seconds = 0.0;      // Time since the gateway received it.
        std::string effective_status;  // "offline" once the vehicle goes silent.
    };

    struct Counts {
        std::size_t total = 0;
        std::size_t moving = 0;
        std::size_t idle = 0;
        std::size_t offline = 0;
        std::size_t active() const { return total - offline; }
    };

    explicit FleetState(std::chrono::seconds offline_after = std::chrono::seconds(30),
                        Now now = [] { return Clock::now(); });

    // Record a report. A report with a lower offset than the stored one is
    // ignored, so two pool threads finishing out of order cannot move a
    // vehicle backwards. `age` is non-zero only when rebuilding from the log.
    void update(const TelemetryEvent& event, std::uint64_t offset,
                std::chrono::nanoseconds age = std::chrono::nanoseconds::zero());

    std::vector<Vehicle> snapshot() const;
    Counts counts() const;
    std::string to_json() const;

private:
    struct Entry {
        TelemetryEvent event;
        std::uint64_t offset = 0;
        Clock::time_point received_at;
    };

    Vehicle describe(const Entry& entry, Clock::time_point now) const;

    std::chrono::seconds offline_after;
    Now now;
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, Entry> vehicles;
};
