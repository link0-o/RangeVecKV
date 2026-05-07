#include "infra/trace_context.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace kvai::infra {

std::string TraceContext::GenerateTraceId() {
    static std::atomic<std::uint64_t> counter{0};
    static thread_local std::mt19937_64 generator(std::random_device{}());

    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
    const auto random_value = generator();

    std::ostringstream stream;
    stream << std::hex << now << '-' << sequence << '-' << random_value;
    return stream.str();
}

}  // namespace kvai::infra