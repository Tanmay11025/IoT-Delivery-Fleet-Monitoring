#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    struct Stats {
        std::size_t worker_count;
        std::size_t busy_threads;
        std::size_t queue_length;
        std::size_t queue_high_water;
    };

    explicit ThreadPool(std::size_t worker_count, std::size_t queue_capacity)
        : queue_capacity(queue_capacity) {
        workers.reserve(worker_count);
        try {
            for (std::size_t index = 0; index < worker_count; ++index) {
                workers.emplace_back(&ThreadPool::worker_loop, this);
            }
        } catch (...) {
            begin_shutdown();
            join();
            throw;
        }
    }

    ~ThreadPool() {
        begin_shutdown();
        join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || tasks.size() >= queue_capacity) return false;
            tasks.push(std::move(task));
            queue_high_water = std::max(queue_high_water, tasks.size());
        }
        condition.notify_one();
        return true;
    }

    void begin_shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        condition.notify_all();
    }

    void join() {
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex);
        return {workers.size(), busy_threads.load(std::memory_order_relaxed), tasks.size(),
            queue_high_water};
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping || !tasks.empty(); });
                if (stopping && tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop();
                busy_threads.fetch_add(1, std::memory_order_relaxed);
            }
            try {
                task();
            } catch (...) {
            }
            busy_threads.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    const std::size_t queue_capacity;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::queue<std::function<void()>> tasks;
    std::size_t queue_high_water = 0;
    std::vector<std::thread> workers;
    std::atomic<std::size_t> busy_threads{0};
    bool stopping = false;
};