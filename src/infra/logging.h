#pragma once

#include <initializer_list>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>

namespace kvai::infra {

enum class LogLevel {
    kDebug,
    kInfo,
    kWarn,
    kError,
};

class Logger {
public:
    static Logger& Instance();

    void SetLevel(LogLevel level);
    void Log(LogLevel level,
             const std::string& component,
             const std::string& message,
             std::initializer_list<std::pair<std::string, std::string>> fields = {});

private:
    Logger() = default;

    [[nodiscard]] static const char* ToString(LogLevel level);

    std::mutex mutex_;
    LogLevel level_ = LogLevel::kInfo;
};

}  // namespace kvai::infra