#include "infra/config.h"

#include <cstdlib>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>

namespace kvai::infra {

namespace {

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }

    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }

    return value;
}

bool ParseBool(std::string value) {
    value = Trim(std::move(value));
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::string ExpandEnvironmentVariables(const std::string& value) {
    std::string expanded;
    expanded.reserve(value.size());

    for (std::size_t index = 0; index < value.size();) {
        if (value[index] == '$' && index + 1 < value.size() && value[index + 1] == '{') {
            const auto end = value.find('}', index + 2);
            if (end == std::string::npos) {
                expanded.append(value.substr(index));
                break;
            }

            const auto variable_name = value.substr(index + 2, end - index - 2);
            const char* variable_value = std::getenv(variable_name.c_str());
            if (variable_value != nullptr) {
                expanded.append(variable_value);
            }
            index = end + 1;
            continue;
        }

        expanded.push_back(value[index]);
        ++index;
    }

    return expanded;
}

}  // namespace

StatusOr<ServerConfig> ConfigLoader::LoadFromFile(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return Status::NotFound("config file not found: " + path);
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        const auto comment_position = line.find('#');
        if (comment_position != std::string::npos) {
            line = line.substr(0, comment_position);
        }

        line = Trim(std::move(line));
        if (line.empty()) {
            continue;
        }

        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }

        auto key = Trim(line.substr(0, separator));
        auto value = ExpandEnvironmentVariables(Trim(line.substr(separator + 1)));
        values[std::move(key)] = std::move(value);
    }

    ServerConfig config;

    if (const auto iterator = values.find("server.host"); iterator != values.end()) {
        config.host = iterator->second;
    }
    if (const auto iterator = values.find("server.port"); iterator != values.end()) {
        config.port = static_cast<std::uint16_t>(std::stoul(iterator->second));
    }
    if (const auto iterator = values.find("server.worker_threads"); iterator != values.end()) {
        config.worker_threads = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("server.graceful_shutdown_timeout_ms"); iterator != values.end()) {
        config.graceful_shutdown_timeout_ms = static_cast<std::uint32_t>(std::stoul(iterator->second));
    }
    if (const auto iterator = values.find("search.max_top_k"); iterator != values.end()) {
        config.max_top_k = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("gateway.rate_limit_per_second"); iterator != values.end()) {
        config.rate_limit_per_second = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("gateway.require_api_key"); iterator != values.end()) {
        config.require_api_key = ParseBool(iterator->second);
    }
    if (const auto iterator = values.find("gateway.api_key"); iterator != values.end()) {
        config.api_key = iterator->second;
    }
    if (const auto iterator = values.find("ai.timeout_ms"); iterator != values.end()) {
        config.ai_timeout_ms = static_cast<std::uint32_t>(std::stoul(iterator->second));
    }
    if (const auto iterator = values.find("ai.model_path"); iterator != values.end()) {
        config.model_path = iterator->second;
    }
    if (const auto iterator = values.find("ai.embedding_dimensions"); iterator != values.end()) {
        config.embedding_dimensions = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("storage.default_collection"); iterator != values.end()) {
        config.default_collection = iterator->second;
    }
    if (const auto iterator = values.find("cluster.discovery_backend"); iterator != values.end()) {
        config.discovery_backend = iterator->second;
    }
    if (const auto iterator = values.find("cluster.node_id"); iterator != values.end()) {
        config.node_id = iterator->second;
    }
    if (const auto iterator = values.find("cluster.replication_factor"); iterator != values.end()) {
        config.replication_factor = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("cluster.nodes"); iterator != values.end()) {
        config.cluster_nodes = iterator->second;
    }
    if (const auto iterator = values.find("ai.backend"); iterator != values.end()) {
        config.ai_backend = iterator->second;
    }
    if (const auto iterator = values.find("storage.backend"); iterator != values.end()) {
        config.storage_backend = iterator->second;
    }
    if (const auto iterator = values.find("search.backend"); iterator != values.end()) {
        config.search_backend = iterator->second;
    }
    if (const auto iterator = values.find("storage.wal_path"); iterator != values.end()) {
        config.wal_path = iterator->second;
    }
    if (const auto iterator = values.find("storage.snapshot_path"); iterator != values.end()) {
        config.snapshot_path = iterator->second;
    }
    if (const auto iterator = values.find("storage.db_path"); iterator != values.end()) {
        config.db_path = iterator->second;
    }
    if (const auto iterator = values.find("storage.read_only_mode"); iterator != values.end()) {
        config.read_only_mode = ParseBool(iterator->second);
    }
    if (const auto iterator = values.find("storage.enable_demo_data"); iterator != values.end()) {
        config.enable_demo_data = ParseBool(iterator->second);
    }
    if (const auto iterator = values.find("search.index_path"); iterator != values.end()) {
        config.index_path = iterator->second;
    }
    if (const auto iterator = values.find("gateway.openapi_path"); iterator != values.end()) {
        config.openapi_path = iterator->second;
    }
    if (const auto iterator = values.find("security.tls_mode"); iterator != values.end()) {
        config.tls_mode = iterator->second;
    }
    if (const auto iterator = values.find("log.level"); iterator != values.end()) {
        config.log_level = iterator->second;
    }

    return config;
}

}  // namespace kvai::infra