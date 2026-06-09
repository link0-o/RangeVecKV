#include "infra/metrics.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <cstdlib>
#endif

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
    output << "# HELP kvai_cpu_utilization_ratio Approximate host CPU utilization ratio from load average\n";
    output << "# TYPE kvai_cpu_utilization_ratio gauge\n";
    output << "kvai_cpu_utilization_ratio " << CpuUtilizationRatio() << '\n';
    output << "# HELP kvai_disk_utilization_ratio Current working directory filesystem utilization ratio\n";
    output << "# TYPE kvai_disk_utilization_ratio gauge\n";
    output << "kvai_disk_utilization_ratio " << DiskUtilizationRatio(".") << '\n';
    return output.str();
}

double MetricsRegistry::CpuUtilizationRatio() {
#if defined(__unix__) || defined(__APPLE__)
    double loads[1] = {0.0};
    if (getloadavg(loads, 1) == 1) {
        const auto cores = std::max<unsigned int>(1, std::thread::hardware_concurrency());
        return std::min(1.0, std::max(0.0, loads[0] / static_cast<double>(cores)));
    }
#endif
    return 0.0;
}

double MetricsRegistry::DiskUtilizationRatio(const std::string& path) {
    std::error_code error;
    const auto info = std::filesystem::space(path.empty() ? "." : path, error);
    if (error || info.capacity == 0) {
        return 0.0;
    }
    const auto used = static_cast<double>(info.capacity - info.available);
    return std::min(1.0, std::max(0.0, used / static_cast<double>(info.capacity)));
}

}  // namespace kvai::infra
