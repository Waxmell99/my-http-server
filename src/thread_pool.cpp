#include "common/log.h"
#include "concurrency/thread_pool.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace personal_cloud {

ThreadPool::ThreadPool(
    std::size_t worker_count,
    std::size_t maximum_queue_size)
    : maximum_queue_size_(maximum_queue_size) {
    if (worker_count == 0) {
        throw std::invalid_argument("worker_count must be greater than zero");
    }

    if (maximum_queue_size == 0) {
        throw std::invalid_argument(
            "maximum_queue_size must be greater than zero");
    }

    workers_.reserve(worker_count);

    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] {
                worker_loop();
            });
        }
    } catch (...) {
        stop_and_join();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    stop_and_join();
}

bool ThreadPool::submit(std::function<void()> task) {
    if (!task) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);

        if (stopping_ || tasks_.size() >= maximum_queue_size_) {
            return false;
        }

        tasks_.push(std::move(task));
    }

    condition_.notify_one();
    return true;
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });

            if (stopping_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        } catch (const std::exception& error) {
            write_log(
                std::cerr, "ThreadPool task failed: ", error.what(), '\n');
        } catch (...) {
            write_log(
                std::cerr,
                "ThreadPool task failed with an unknown exception.\n");
        }
    }
}

void ThreadPool::stop_and_join() noexcept {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }

    condition_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

}  // namespace personal_cloud
