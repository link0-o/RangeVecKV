#include "infra/metrics.h"

#include <sstream>

namespace kvai::infra {

MetricsRegistry::MetricsRegistry() : started_at_(std::chrono::steady_clock::now()) {}

void MetricsRegistry::IncrementSearchRequests() {
    search_requests_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::IncrementSearchFailures() {
    search_failures_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::IncrementDegradedSearches() {
    degraded_searches_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::IncrementHttpRequests() {
    http_requests_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::ObserveSearchLatency(std::chrono::microseconds latency) {
    total_search_latency_us_.fetch_add(static_cast<std::uint64_t>(latency.count()), std::memory_order_relaxed);
}

std::uint64_t MetricsRegistry::search_requests() const {
    return search_requests_.load(std::memory_order_relaxed);
}

std::uint64_t MetricsRegistry::search_failures() const {
    return search_failures_.load(std::memory_order_relaxed);
}

std::uint64_t MetricsRegistry::degraded_searches() const {
    return degraded_searches_.load(std::memory_order_relaxed);
}

std::uint64_t MetricsRegistry::http_requests() const {
    return http_requests_.load(std::memory_order_relaxed);
}

std::string MetricsRegistry::RenderPrometheus() const {
    const auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at_).count();
    const auto total_requests = search_requests();
    const auto total_latency = total_search_latency_us_.load(std::memory_order_relaxed);
    const double average_latency = total_requests == 0 ? 0.0 : static_cast<double>(total_latency) / static_cast<double>(total_requests);

    std::ostringstream output;
    output << "# HELP kvai_search_requests_total Total semantic search requests\n";
    output << "# TYPE kvai_search_requests_total counter\n";
    output << "kvai_search_requests_total " << total_requests << '\n';
    output << "# HELP kvai_search_failures_total Total failed semantic search requests\n";
    output << "# TYPE kvai_search_failures_total counter\n";
    output << "kvai_search_failures_total " << search_failures() << '\n';
    output << "# HELP kvai_search_degraded_total Total degraded semantic search requests\n";
    output << "# TYPE kvai_search_degraded_total counter\n";
    output << "kvai_search_degraded_total " << degraded_searches() << '\n';
    output << "# HELP kvai_http_requests_total Total HTTP requests\n";
    output << "# TYPE kvai_http_requests_total counter\n";
    output << "kvai_http_requests_total " << http_requests() << '\n';
    output << "# HELP kvai_search_latency_average_us Average semantic search latency in microseconds\n";
    output << "# TYPE kvai_search_latency_average_us gauge\n";
    output << "kvai_search_latency_average_us " << average_latency << '\n';
    output << "# HELP kvai_uptime_seconds Process uptime in seconds\n";
    output << "# TYPE kvai_uptime_seconds gauge\n";
    output << "kvai_uptime_seconds " << uptime_seconds << '\n';
    return output.str();
}

}  // namespace kvai::infra