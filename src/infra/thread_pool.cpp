#include "infra/thread_pool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(KVAI_HAVE_TBB)
#include <tbb/task_arena.h>
#endif

namespace kvai::infra {

namespace {

std::size_t EffectiveWorkerCount(std::size_t worker_count) {
    return std::max<std::size_t>(1, worker_count);
}

std::size_t EffectiveQueueCapacity(std::size_t worker_count, std::size_t queue_capacity) {
    return queue_capacity == 0 ? std::max<std::size_t>(64, EffectiveWorkerCount(worker_count) * 1024) : queue_capacity;
}

class FallbackThreadPoolBackend final : public ThreadPoolBackend {
public:
    FallbackThreadPoolBackend(std::size_t worker_count, std::size_t queue_capacity)
        : queue_capacity_(EffectiveQueueCapacity(worker_count, queue_capacity)) {
        const auto effective_worker_count = EffectiveWorkerCount(worker_count);
        workers_.reserve(effective_worker_count);
        for (std::size_t index = 0; index < effective_worker_count; ++index) {
            workers_.emplace_back([this]() { WorkerLoop(); });
        }
    }

    ~FallbackThreadPoolBackend() override {
        Shutdown();
    }

    bool SubmitTask(std::function<void()> task, std::string& error_message) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                error_message = "thread pool is stopped";
                return false;
            }
            if (tasks_.size() >= queue_capacity_) {
                error_message = "thread pool queue is full";
                return false;
            }

            tasks_.push(std::move(task));
            submitted_tasks_.fetch_add(1, std::memory_order_relaxed);
        }

        condition_.notify_one();
        return true;
    }

    void Shutdown() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }

        condition_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        workers_.clear();
    }

    std::size_t PendingTasks() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    std::uint64_t SubmittedTasks() const override {
        return submitted_tasks_.load(std::memory_order_relaxed);
    }

    std::uint64_t CompletedTasks() const override {
        return completed_tasks_.load(std::memory_order_relaxed);
    }

    std::size_t WorkerCount() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return workers_.size();
    }

private:
    void WorkerLoop() {
        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });

                if (stopping_ && tasks_.empty()) {
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }

            try {
                task();
            } catch (...) {
                // Packaged tasks capture exceptions into their futures. This guard
                // protects the worker loop if a raw task is ever added internally.
            }
            completed_tasks_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    std::size_t queue_capacity_ = 0;
    std::atomic<std::uint64_t> submitted_tasks_{0};
    std::atomic<std::uint64_t> completed_tasks_{0};
    bool stopping_ = false;
};

#if defined(KVAI_HAVE_TBB)
class TbbThreadPoolBackend final : public ThreadPoolBackend {
public:
    TbbThreadPoolBackend(std::size_t worker_count, std::size_t queue_capacity)
        : worker_count_(EffectiveWorkerCount(worker_count)),
          queue_capacity_(EffectiveQueueCapacity(worker_count, queue_capacity)),
          arena_(static_cast<int>(worker_count_), 0) {
        arena_.initialize();
    }

    ~TbbThreadPoolBackend() noexcept override {
        Shutdown();
    }

    bool SubmitTask(std::function<void()> task, std::string& error_message) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                error_message = "thread pool is stopped";
                return false;
            }
            if (pending_tasks_.load(std::memory_order_relaxed) >= queue_capacity_) {
                error_message = "thread pool queue is full";
                return false;
            }

            pending_tasks_.fetch_add(1, std::memory_order_relaxed);
            submitted_tasks_.fetch_add(1, std::memory_order_relaxed);
        }

        try {
            arena_.enqueue([this, task = std::move(task)]() {
                try {
                    task();
                } catch (...) {
                    // Keep backend workers alive; packaged_task records user exceptions.
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    completed_tasks_.fetch_add(1, std::memory_order_relaxed);
                    pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
                    condition_.notify_all();
                }
            });
        } catch (const std::exception& error) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
                submitted_tasks_.fetch_sub(1, std::memory_order_relaxed);
                error_message = error.what();
                condition_.notify_all();
            }
            return false;
        }
        return true;
    }

    void Shutdown() override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!stopping_) {
            stopping_ = true;
        }
        condition_.wait(lock, [this]() { return pending_tasks_.load(std::memory_order_relaxed) == 0; });
        if (arena_active_) {
            arena_.terminate();
            arena_active_ = false;
        }
    }

    std::size_t PendingTasks() const override {
        return pending_tasks_.load(std::memory_order_relaxed);
    }

    std::uint64_t SubmittedTasks() const override {
        return submitted_tasks_.load(std::memory_order_relaxed);
    }

    std::uint64_t CompletedTasks() const override {
        return completed_tasks_.load(std::memory_order_relaxed);
    }

    std::size_t WorkerCount() const override {
        return worker_count_;
    }

private:
    std::size_t worker_count_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    tbb::task_arena arena_;
    std::atomic<std::size_t> pending_tasks_{0};
    std::atomic<std::uint64_t> submitted_tasks_{0};
    std::atomic<std::uint64_t> completed_tasks_{0};
    bool stopping_ = false;
    bool arena_active_ = true;
};
#endif

std::unique_ptr<ThreadPoolBackend> CreateThreadPoolBackend(std::size_t worker_count, std::size_t queue_capacity) {
#if defined(KVAI_HAVE_TBB)
    return std::make_unique<TbbThreadPoolBackend>(worker_count, queue_capacity);
#else
    return std::make_unique<FallbackThreadPoolBackend>(worker_count, queue_capacity);
#endif
}

}  // namespace

ThreadPool::ThreadPool(std::size_t worker_count, std::size_t queue_capacity)
    : backend_(CreateThreadPoolBackend(worker_count, queue_capacity)) {}

ThreadPool::~ThreadPool() {
    Shutdown();
}

void ThreadPool::Shutdown() {
    backend_->Shutdown();
}

std::size_t ThreadPool::PendingTasks() const {
    return backend_->PendingTasks();
}

std::uint64_t ThreadPool::SubmittedTasks() const {
    return backend_->SubmittedTasks();
}

std::uint64_t ThreadPool::CompletedTasks() const {
    return backend_->CompletedTasks();
}

std::size_t ThreadPool::WorkerCount() const {
    return backend_->WorkerCount();
}

}  // namespace kvai::infra
