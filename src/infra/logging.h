#pragma once

#include <map>
#include <string>

#include "infra/config.h"

namespace kvai::infra::log {

/// Initialize the logging subsystem from server config.
/// Must be called once from main() before any log output.
/// When spdlog is available, configures async logger with console + rotating
/// file sinks. When spdlog is unavailable, falls back to synchronous
/// stdout/stderr output.
void ConfigureLogger(const ServerConfig& config);

/// Set the global log level. Valid values: "debug", "info", "warn", "error".
void SetLevel(const std::string& level);

void Debug(const std::string& component, const std::string& message,
           const std::map<std::string, std::string>& fields = {});
void Info(const std::string& component, const std::string& message,
          const std::map<std::string, std::string>& fields = {});
void Warn(const std::string& component, const std::string& message,
          const std::map<std::string, std::string>& fields = {});
void Error(const std::string& component, const std::string& message,
           const std::map<std::string, std::string>& fields = {});

}  // namespace kvai::infra::log
