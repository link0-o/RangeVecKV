#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "infra/thread_pool.h"

namespace {

bool Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

template <typename Future>
bool ExpectFutureThrows(Future& future, const std::string& expected_message) {
    try {
        future.get();
    } catch (const std::exception& error) {
        return std::string(error.what()).find(expected_message) != std::string::npos;
    }
    return false;
}

}  // namespace

int main() {
    int failures = 0;

    {
        kvai::infra::ThreadPool pool(2, 16);
        std::vector<std::future<int>> futures;
        for (int index = 0; index < 8; ++index) {
            futures.push_back(pool.Submit([index]() { return index * 2; }));
        }

        int sum = 0;
        for (auto& future : futures) {
            sum += future.get();
        }

        if (!Expect(sum == 56, "thread pool returned unexpected task results")) ++failures;
        if (!Expect(pool.SubmittedTasks() == 8, "submitted task count mismatch")) ++failures;
        for (int spin = 0; spin < 100 && pool.CompletedTasks() < 8; ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!Expect(pool.CompletedTasks() == 8, "completed task count mismatch")) ++failures;
        if (!Expect(pool.WorkerCount() >= 1, "worker count should be positive")) ++failures;
        pool.Shutdown();
    }

    {
        kvai::infra::ThreadPool pool(1, 1);
        std::promise<void> release_task;
        auto release_future = release_task.get_future().share();
        auto first = pool.Submit([release_future]() mutable {
            release_future.wait();
            return 1;
        });

        std::vector<std::future<int>> futures;
        for (int index = 0; index < 64; ++index) {
            futures.push_back(pool.Submit([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return 2;
            }));
        }

        bool saw_queue_full = false;
        for (auto& future : futures) {
            if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                saw_queue_full = ExpectFutureThrows(future, "queue is full") || saw_queue_full;
            }
        }
        release_task.set_value();
        (void)first.get();
        for (auto& future : futures) {
            if (future.valid() && future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) {
                try {
                    (void)future.get();
                } catch (...) {
                }
            }
        }
        if (!Expect(saw_queue_full, "queue capacity should reject at least one overloaded task")) ++failures;
        pool.Shutdown();
    }

    {
        kvai::infra::ThreadPool pool(1, 4);
        auto throwing = pool.Submit([]() -> int { throw std::runtime_error("boom"); });
        if (!Expect(ExpectFutureThrows(throwing, "boom"), "task exception should be visible through future")) ++failures;

        auto after_exception = pool.Submit([]() { return 7; });
        if (!Expect(after_exception.get() == 7, "worker should continue after task exception")) ++failures;
        pool.Shutdown();
    }

    {
        kvai::infra::ThreadPool pool(1, 4);
        pool.Shutdown();
        auto rejected = pool.Submit([]() { return 1; });
        if (!Expect(ExpectFutureThrows(rejected, "stopped"), "submit after shutdown should fail through future")) ++failures;
    }

    return failures == 0 ? 0 : 1;
}
