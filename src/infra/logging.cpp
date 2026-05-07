#include "infra/logging.h"

#include <chrono>
#include <iostream>

namespace kvai::infra {

namespace {

bool ShouldEmit(LogLevel current, LogLevel incoming) {
    return static_cast<int>(incoming) >= static_cast<int>(current);
}

}  // namespace

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

void Logger::Log(LogLevel level,
                 const std::string& component,
                 const std::string& message,
                 std::initializer_list<std::pair<std::string, std::string>> fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ShouldEmit(level_, level)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::ostream& stream = level == LogLevel::kError ? std::cerr : std::clog;
    stream << "ts=" << unix_seconds << " level=" << ToString(level) << " component=" << component << " msg=\"" << message
           << "\"";

    for (const auto& [key, value] : fields) {
        stream << ' ' << key << "=\"" << value << "\"";
    }

    stream << std::endl;
}

const char* Logger::ToString(LogLevel level) {
    switch (level) {
    case LogLevel::kDebug:
        return "DEBUG";
    case LogLevel::kInfo:
        return "INFO";
    case LogLevel::kWarn:
        return "WARN";
    case LogLevel::kError:
        return "ERROR";
    }

    return "UNKNOWN";
}

}  // namespace kvai::infra