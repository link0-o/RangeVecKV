#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace kvai::infra {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename Function>
    auto Submit(Function&& function) -> std::future<typename std::invoke_result_t<Function>> {
        using ResultType = typename std::invoke_result_t<Function>;

        auto task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<Function>(function));
        std::future<ResultType> future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push([task]() { (*task)(); });
        }

        condition_.notify_one();
        return future;
    }

    void Shutdown();

private:
    void WorkerLoop();

    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

}  // namespace kvai::infra