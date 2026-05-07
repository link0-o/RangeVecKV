#include "gateway/interceptors.h"

#include "infra/trace_context.h"

namespace kvai::gateway {

std::string TraceInjector::EnsureTraceId(const std::string& trace_id) {
    return trace_id.empty() ? kvai::infra::TraceContext::GenerateTraceId() : trace_id;
}

FixedWindowRateLimiter::FixedWindowRateLimiter(std::size_t max_requests_per_second)
    : max_requests_per_second_(max_requests_per_second), window_start_(std::chrono::steady_clock::now()) {}

bool FixedWindowRateLimiter::Allow() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start_ >= std::chrono::seconds(1)) {
        window_start_ = now;
        request_count_ = 0;
    }

    if (request_count_ >= max_requests_per_second_) {
        return false;
    }

    ++request_count_;
    return true;
}

}  // namespace kvai::gateway