#include "infra/logging.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

#if defined(KVAI_HAVE_SPDLOG)
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#endif

namespace kvai::infra::log {

namespace {

std::string FormatFields(const std::map<std::string, std::string>& fields) {
    if (fields.empty()) {
        return {};
    }
    std::string result;
    for (const auto& [key, value] : fields) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += key + "=\"" + value + "\"";
    }
    return result;
}

#if defined(KVAI_HAVE_SPDLOG)

spdlog::level::level_enum ParseSpdlogLevel(const std::string& level) {
    auto lowered = level;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered == "debug") {
        return spdlog::level::debug;
    }
    if (lowered == "warn" || lowered == "warning") {
        return spdlog::level::warn;
    }
    if (lowered == "error" || lowered == "err") {
        return spdlog::level::err;
    }
    return spdlog::level::info;
}

bool g_logger_configured = false;

#else

enum class FallbackLevel { kDebug, kInfo, kWarn, kError };

FallbackLevel g_fallback_level = FallbackLevel::kInfo;
std::mutex g_fallback_mutex;

bool ShouldEmit(FallbackLevel incoming) {
    return static_cast<int>(incoming) >= static_cast<int>(g_fallback_level);
}

FallbackLevel ParseFallbackLevel(const std::string& level) {
    auto lowered = level;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered == "debug") {
        return FallbackLevel::kDebug;
    }
    if (lowered == "warn" || lowered == "warning") {
        return FallbackLevel::kWarn;
    }
    if (lowered == "error" || lowered == "err") {
        return FallbackLevel::kError;
    }
    return FallbackLevel::kInfo;
}

const char* LevelString(FallbackLevel level) {
    switch (level) {
    case FallbackLevel::kDebug:
        return "DEBUG";
    case FallbackLevel::kInfo:
        return "INFO";
    case FallbackLevel::kWarn:
        return "WARN";
    case FallbackLevel::kError:
        return "ERROR";
    }
    return "UNKNOWN";
}

void FallbackLog(FallbackLevel level, const std::string& component,
                 const std::string& message, const std::map<std::string, std::string>& fields) {
    std::lock_guard<std::mutex> lock(g_fallback_mutex);
    if (!ShouldEmit(level)) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::ostream& stream = (level == FallbackLevel::kError) ? std::cerr : std::clog;
    stream << "ts=" << seconds << " level=" << LevelString(level) << " component=" << component
           << " msg=\"" << message << "\"";
    const auto formatted = FormatFields(fields);
    if (!formatted.empty()) {
        stream << " " << formatted;
    }
    stream << std::endl;
}

#endif

}  // namespace

void ConfigureLogger(const ServerConfig& config) {
#if defined(KVAI_HAVE_SPDLOG)
    if (g_logger_configured) {
        return;
    }

    try {
        spdlog::init_thread_pool(8192, 1);

        std::vector<spdlog::sink_ptr> sinks;

#ifndef NDEBUG
        // Debug build: also log to console for local development
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif

        auto log_file_path = config.log_file_path;
#ifdef NDEBUG
        if (log_file_path.empty()) {
            log_file_path = "./logs/rangeveckv.log";
        }
#endif

        if (!log_file_path.empty()) {
            const auto parent = std::filesystem::path(log_file_path).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            const auto max_size = static_cast<std::size_t>(config.log_file_max_size_mb) * 1024ULL * 1024ULL;
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file_path, max_size, config.log_file_max_files));
        }

        auto logger = std::make_shared<spdlog::async_logger>(
            "rangeveckv", sinks.begin(), sinks.end(), spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);

        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
        spdlog::set_default_logger(logger);
        spdlog::set_level(ParseSpdlogLevel(config.log_level));
        spdlog::flush_every(std::chrono::seconds(3));

        g_logger_configured = true;
    } catch (const std::exception& error) {
        std::cerr << "Failed to configure spdlog: " << error.what() << std::endl;
    }
#else
    g_fallback_level = ParseFallbackLevel(config.log_level);
#endif
}

void SetLevel(const std::string& level) {
#if defined(KVAI_HAVE_SPDLOG)
    spdlog::set_level(ParseSpdlogLevel(level));
#else
    g_fallback_level = ParseFallbackLevel(level);
#endif
}

void Debug(const std::string& component, const std::string& message,
           const std::map<std::string, std::string>& fields) {
#if defined(KVAI_HAVE_SPDLOG)
    const auto formatted = FormatFields(fields);
    if (formatted.empty()) {
        spdlog::debug("[{}] {}", component, message);
    } else {
        spdlog::debug("[{}] {} {}", component, message, formatted);
    }
#else
    FallbackLog(FallbackLevel::kDebug, component, message, fields);
#endif
}

void Info(const std::string& component, const std::string& message,
          const std::map<std::string, std::string>& fields) {
#if defined(KVAI_HAVE_SPDLOG)
    const auto formatted = FormatFields(fields);
    if (formatted.empty()) {
        spdlog::info("[{}] {}", component, message);
    } else {
        spdlog::info("[{}] {} {}", component, message, formatted);
    }
#else
    FallbackLog(FallbackLevel::kInfo, component, message, fields);
#endif
}

void Warn(const std::string& component, const std::string& message,
          const std::map<std::string, std::string>& fields) {
#if defined(KVAI_HAVE_SPDLOG)
    const auto formatted = FormatFields(fields);
    if (formatted.empty()) {
        spdlog::warn("[{}] {}", component, message);
    } else {
        spdlog::warn("[{}] {} {}", component, message, formatted);
    }
#else
    FallbackLog(FallbackLevel::kWarn, component, message, fields);
#endif
}

void Error(const std::string& component, const std::string& message,
           const std::map<std::string, std::string>& fields) {
#if defined(KVAI_HAVE_SPDLOG)
    const auto formatted = FormatFields(fields);
    if (formatted.empty()) {
        spdlog::error("[{}] {}", component, message);
    } else {
        spdlog::error("[{}] {} {}", component, message, formatted);
    }
#else
    FallbackLog(FallbackLevel::kError, component, message, fields);
#endif
}

}  // namespace kvai::infra::log
