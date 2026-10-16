#pragma once

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

template <typename T>
class CircularBuffer {
public:
    // T must be default-constructible for the backing vector and copyable for get_recent().
    // This container is not thread-safe; callers must provide synchronization.
    explicit CircularBuffer(std::size_t capacity) : entries(capacity) {
        if (capacity == 0) throw std::invalid_argument("circular buffer capacity must be positive");
    }

    void push(T value) {
        entries[write_index] = std::move(value);
        write_index = (write_index + 1) % entries.size();
        if (count < entries.size()) ++count;
    }

    std::vector<T> get_recent(std::size_t count) const {
        const std::size_t result_size = count < this->count ? count : this->count;
        std::vector<T> result;
        result.reserve(result_size);
        const std::size_t first = (write_index + entries.size() - result_size) % entries.size();
        for (std::size_t index = 0; index < result_size; ++index) {
            result.push_back(entries[(first + index) % entries.size()]);
        }
        return result;
    }

    std::size_t capacity() const { return entries.size(); }
    std::size_t size() const { return count; }

private:
    std::vector<T> entries;
    std::size_t write_index = 0;
    std::size_t count = 0;
};