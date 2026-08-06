#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace personal_cloud {

class ThreadPool final {
public:
    ThreadPool(
        std::size_t worker_count,
        std::size_t maximum_queue_size);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // 任务成功进入队列时返回 true；队列已满或正在停止时返回 false。
    bool submit(std::function<void()> task);

private:
    void worker_loop();
    void stop_and_join() noexcept;

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t maximum_queue_size_;
    bool stopping_{false};
};

}  // namespace personal_cloud
