#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace kvai::infra {

class MetricsRegistry {
public:
    MetricsRegistry();

    void IncrementSearchRequests();
    void IncrementSearchFailures();
    void IncrementDegradedSearches();
    void IncrementHttpRequests();
    void ObserveSearchLatency(std::chrono::microseconds latency);

    [[nodiscard]] std::uint64_t search_requests() const;
    [[nodiscard]] std::uint64_t search_failures() const;
    [[nodiscard]] std::uint64_t degraded_searches() const;
    [[nodiscard]] std::uint64_t http_requests() const;
    [[nodiscard]] std::string RenderPrometheus() const;

private:
    std::chrono::steady_clock::time_point started_at_;
    std::atomic<std::uint64_t> search_requests_{0};
    std::atomic<std::uint64_t> search_failures_{0};
    std::atomic<std::uint64_t> degraded_searches_{0};
    std::atomic<std::uint64_t> http_requests_{0};
    std::atomic<std::uint64_t> total_search_latency_us_{0};
};

}  // namespace kvai::infra