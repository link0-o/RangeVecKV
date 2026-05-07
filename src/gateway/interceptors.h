#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

namespace kvai::gateway {

class TraceInjector {
public:
    static std::string EnsureTraceId(const std::string& trace_id);
};

class FixedWindowRateLimiter {
public:
    explicit FixedWindowRateLimiter(std::size_t max_requests_per_second);

    bool Allow();

private:
    std::size_t max_requests_per_second_;
    std::size_t request_count_ = 0;
    std::chrono::steady_clock::time_point window_start_;
    std::mutex mutex_;
};

}  // namespace kvai::gateway