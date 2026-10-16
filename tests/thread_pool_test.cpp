#include "network/thread_pool.h"

#include <cassert>
#include <chrono>
#include <future>
#include "test_runner.h"

void pool_bounds_queue_and_drains_accepted_tasks() {
    ThreadPool pool(1, 1);
    std::promise<void> task_started;
    std::promise<void> release_task;
    const auto release = release_task.get_future().share();
    bool release_signaled = false;
    struct ReleaseOnExit {
        std::promise<void>& release;
        bool& signaled;
        ~ReleaseOnExit() {
            if (!signaled) release.set_value();
        }
        void signal() {
            if (!signaled) {
                release.set_value();
                signaled = true;
            }
        }
    } release_on_exit{release_task, release_signaled};
    std::promise<void> queued_task_done;
    auto queued_done = queued_task_done.get_future();

    assert(pool.submit([&] {
        task_started.set_value();
        release.wait();
    }));
    task_started.get_future().wait();
    assert(pool.submit([&] { queued_task_done.set_value(); }));
    assert(!pool.submit([] {}));

    const auto stats = pool.stats();
    assert(stats.worker_count == 1);
    assert(stats.busy_threads == 1);
    assert(stats.queue_length == 1);

    pool.begin_shutdown();
    assert(!pool.submit([] {}));
    release_on_exit.signal();
    pool.join();
    assert(queued_done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
}

void pool_survives_task_exceptions() {
    ThreadPool pool(1, 2);
    std::promise<void> completed;
    auto result = completed.get_future();
    assert(pool.submit([] { throw 1; }));
    assert(pool.submit([&] { completed.set_value(); }));
    assert(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
}

int main(int argc, char* argv[]) {
    TestRunner runner(argc, argv);
    runner.run("pool_bounds_queue_and_drains_accepted_tasks",
               pool_bounds_queue_and_drains_accepted_tasks);
    runner.run("pool_survives_task_exceptions", pool_survives_task_exceptions);
    return runner.finish();
}