#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <atomic>

namespace kvai::infra {

class ThreadPoolBackend {
public:
    virtual ~ThreadPoolBackend() = default;

    virtual bool SubmitTask(std::function<void()> task, std::string& error_message) = 0;
    virtual void Shutdown() = 0;
    [[nodiscard]] virtual std::size_t PendingTasks() const = 0;
    [[nodiscard]] virtual std::uint64_t SubmittedTasks() const = 0;
    [[nodiscard]] virtual std::uint64_t CompletedTasks() const = 0;
    [[nodiscard]] virtual std::size_t WorkerCount() const = 0;
};

class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count, std::size_t queue_capacity = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename Function>
    auto Submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
        using ResultType = std::invoke_result_t<Function>;

        auto task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<Function>(function));
        std::future<ResultType> future = task->get_future();

        std::string error_message;
        if (!backend_->SubmitTask([task]() { (*task)(); }, error_message)) {
            std::promise<ResultType> rejected;
            auto rejected_future = rejected.get_future();
            rejected.set_exception(std::make_exception_ptr(std::runtime_error(error_message)));
            return rejected_future;
        }

        return future;
    }

    void Shutdown();
    [[nodiscard]] std::size_t PendingTasks() const;
    [[nodiscard]] std::uint64_t SubmittedTasks() const;
    [[nodiscard]] std::uint64_t CompletedTasks() const;
    [[nodiscard]] std::size_t WorkerCount() const;

private:
    std::unique_ptr<ThreadPoolBackend> backend_;
};

}  // namespace kvai::infra
