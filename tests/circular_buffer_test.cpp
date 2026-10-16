#include "storage/circular_buffer.h"

#include <cassert>
#include <string>
#include <vector>
#include "test_runner.h"

void circular_buffer_preserves_order_before_wraparound() {
    CircularBuffer<std::string> buffer(3);
    buffer.push("one");
    buffer.push("two");
    const auto recent = buffer.get_recent(3);
    assert(recent.size() == 2);
    assert(recent[0] == "one");
    assert(recent[1] == "two");
}

void circular_buffer_returns_last_values_after_wraparound() {
    CircularBuffer<int> buffer(3);
    for (int value = 1; value <= 5; ++value) buffer.push(value);
    const auto recent = buffer.get_recent(3);
    assert((recent == std::vector<int>{3, 4, 5}));
    assert((buffer.get_recent(2) == std::vector<int>{4, 5}));
}

void circular_buffer_handles_exact_capacity_and_zero_count() {
    CircularBuffer<int> buffer(3);
    buffer.push(4);
    buffer.push(5);
    buffer.push(6);
    assert(buffer.size() == buffer.capacity());
    assert((buffer.get_recent(3) == std::vector<int>{4, 5, 6}));
    assert(buffer.get_recent(0).empty());
    assert((buffer.get_recent(20) == std::vector<int>{4, 5, 6}));
}

void circular_buffer_supports_capacity_one() {
    CircularBuffer<int> buffer(1);
    buffer.push(10);
    assert((buffer.get_recent(1) == std::vector<int>{10}));
    buffer.push(11);
    assert((buffer.get_recent(1) == std::vector<int>{11}));
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("circular_buffer_preserves_order_before_wraparound",
               circular_buffer_preserves_order_before_wraparound);
    runner.run("circular_buffer_returns_last_values_after_wraparound",
               circular_buffer_returns_last_values_after_wraparound);
    runner.run("circular_buffer_handles_exact_capacity_and_zero_count",
               circular_buffer_handles_exact_capacity_and_zero_count);
    runner.run("circular_buffer_supports_capacity_one", circular_buffer_supports_capacity_one);
    return runner.finish();
}