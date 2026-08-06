#include "concurrency/thread_pool.h"

#include <atomic>
#include <future>
#include <iostream>
#include <string_view>

namespace {

int failure_count = 0;

void expect(bool condition, std::string_view description) {
    if (condition) {
        std::cout << "[PASS] " << description << '\n';
        return;
    }

    std::cerr << "[FAIL] " << description << '\n';
    ++failure_count;
}

void test_executes_all_tasks() {
    std::atomic<int> completed_tasks{0};
    bool all_submitted = true;

    {
        personal_cloud::ThreadPool pool(4, 128);

        for (int index = 0; index < 100; ++index) {
            all_submitted = pool.submit([&completed_tasks] {
                completed_tasks.fetch_add(1, std::memory_order_relaxed);
            }) && all_submitted;
        }
    }

    expect(all_submitted, "submit all tasks within queue capacity");
    expect(completed_tasks.load(std::memory_order_relaxed) == 100,
           "destructor waits for all submitted tasks");
}

void test_queue_capacity() {
    std::promise<void> worker_started;
    std::future<void> worker_started_future = worker_started.get_future();

    std::promise<void> release_worker;
    std::shared_future<void> release_future =
        release_worker.get_future().share();

    std::atomic<int> queued_task_runs{0};

    {
        personal_cloud::ThreadPool pool(1, 1);

        expect(pool.submit([&worker_started, release_future] {
                   worker_started.set_value();
                   release_future.wait();
               }),
               "submit the running task");

        worker_started_future.wait();

        expect(pool.submit([&queued_task_runs] {
                   queued_task_runs.fetch_add(1, std::memory_order_relaxed);
               }),
               "accept one waiting task at queue capacity");

        expect(!pool.submit([] {}),
               "reject a task when the waiting queue is full");

        release_worker.set_value();
    }

    expect(queued_task_runs.load(std::memory_order_relaxed) == 1,
           "execute the queued task during graceful shutdown");
}

}  // namespace

int main() {
    test_executes_all_tasks();
    test_queue_capacity();

    if (failure_count != 0) {
        std::cerr << failure_count << " test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "All ThreadPool tests passed.\n";
    return 0;
}
