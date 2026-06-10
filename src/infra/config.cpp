#include "infra/config.h"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>

#if defined(KVAI_HAVE_YAML_CPP)
#include <yaml-cpp/yaml.h>
#endif

#include "infra/logging.h"

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

bool ParseBool(const std::string& value) {
    auto lowered = Trim(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
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

#if defined(KVAI_HAVE_YAML_CPP)

/// Recursively expand ${ENV} in all scalar values of a YAML tree.
void ExpandEnvVarsInNode(YAML::Node& node) {
    if (node.IsScalar()) {
        auto value = node.as<std::string>();
        auto expanded = ExpandEnvironmentVariables(value);
        if (expanded != value) {
            node = expanded;
        }
    } else if (node.IsMap()) {
        for (auto iterator = node.begin(); iterator != node.end(); ++iterator) {
            auto key = iterator->first;
            auto val = iterator->second;
            // Keys are const in YAML iteration; only expand values
            ExpandEnvVarsInNode(val);
            // Re-assign the expanded value back (needed for some YAML node types)
            node[key.as<std::string>()] = val;
        }
    } else if (node.IsSequence()) {
        for (std::size_t index = 0; index < node.size(); ++index) {
            auto value = node[index];
            ExpandEnvVarsInNode(value);
            node[index] = value;
        }
    }
}

/// Safely read a string from a YAML node, returning default if missing or wrong type.
std::string ReadString(const YAML::Node& node, const std::string& key, const std::string& default_value) {
    if (!node.IsMap()) {
        return default_value;
    }
    const auto child = node[key];
    if (!child || !child.IsScalar()) {
        return default_value;
    }
    return child.as<std::string>();
}

/// Safely read a bool from a YAML node.
bool ReadBool(const YAML::Node& node, const std::string& key, bool default_value) {
    if (!node.IsMap()) {
        return default_value;
    }
    const auto child = node[key];
    if (!child) {
        return default_value;
    }
    if (child.IsScalar()) {
        return ParseBool(child.as<std::string>());
    }
    return default_value;
}

std::size_t ReadSize(const YAML::Node& node, const std::string& key, std::size_t default_value) {
    if (!node.IsMap()) {
        return default_value;
    }
    const auto child = node[key];
    if (!child) {
        return default_value;
    }
    return child.as<std::size_t>();
}

/// Detect whether a YAML document uses the nested section format.
/// Nested format means the root contains known section keys like "server", "gateway", etc.
bool IsNestedFormat(const YAML::Node& root) {
    if (!root.IsMap()) {
        return false;
    }
    static const char* known_sections[] = {
        "server", "gateway", "ai", "search", "storage", "cluster", "security", "logging"};
    for (const auto* section : known_sections) {
        if (root[section]) {
            return true;
        }
    }
    return false;
}

/// Load config from nested YAML format (yaml-cpp).
ServerConfig LoadNestedConfig(const YAML::Node& root) {
    ServerConfig config;

    // server section
    const auto server = root["server"];
    if (server) {
        config.host = ReadString(server, "host", config.host);
        if (server["port"]) {
            config.port = static_cast<std::uint16_t>(server["port"].as<unsigned int>());
        }
        if (server["worker_threads"]) {
            config.worker_threads = server["worker_threads"].as<std::size_t>();
        }
        if (server["graceful_shutdown_timeout_ms"]) {
            config.graceful_shutdown_timeout_ms = static_cast<std::uint32_t>(server["graceful_shutdown_timeout_ms"].as<unsigned int>());
        }
    }

    // gateway section
    const auto gateway = root["gateway"];
    if (gateway) {
        if (gateway["rate_limit_per_second"]) {
            config.rate_limit_per_second = gateway["rate_limit_per_second"].as<std::size_t>();
        }
        config.openapi_path = ReadString(gateway, "openapi_path", config.openapi_path);
        config.require_api_key = ReadBool(gateway, "require_api_key", config.require_api_key);
        config.api_key = ReadString(gateway, "api_key", config.api_key);
    }

    // ai section
    const auto ai = root["ai"];
    if (ai) {
        if (ai["timeout_ms"]) {
            config.ai_timeout_ms = static_cast<std::uint32_t>(ai["timeout_ms"].as<unsigned int>());
        }
        config.ai_backend = ReadString(ai, "backend", config.ai_backend);
        config.model_path = ReadString(ai, "model_path", config.model_path);
        config.tokenizer_path = ReadString(ai, "tokenizer_path", config.tokenizer_path);
        if (ai["max_tokens"]) {
            config.ai_max_tokens = ai["max_tokens"].as<std::size_t>();
        }
        if (ai["embedding_dimensions"]) {
            config.embedding_dimensions = ai["embedding_dimensions"].as<std::size_t>();
        }
    }

    // search section
    const auto search = root["search"];
    if (search) {
        if (search["max_top_k"]) {
            config.max_top_k = search["max_top_k"].as<std::size_t>();
        }
        config.search_backend = ReadString(search, "backend", config.search_backend);
        config.index_path = ReadString(search, "index_path", config.index_path);
        if (search["faiss_nlist"]) {
            config.search_faiss_nlist = search["faiss_nlist"].as<int>();
        }
        if (search["faiss_nprobe"]) {
            config.search_faiss_nprobe = search["faiss_nprobe"].as<int>();
        }
    }

    // storage section
    const auto storage = root["storage"];
    if (storage) {
        config.default_collection = ReadString(storage, "default_collection", config.default_collection);
        config.storage_backend = ReadString(storage, "backend", config.storage_backend);
        config.read_only_mode = ReadBool(storage, "read_only_mode", config.read_only_mode);
        config.enable_demo_data = ReadBool(storage, "enable_demo_data", config.enable_demo_data);
        config.wal_path = ReadString(storage, "wal_path", config.wal_path);
        config.snapshot_path = ReadString(storage, "snapshot_path", config.snapshot_path);
        config.db_path = ReadString(storage, "db_path", config.db_path);
    }

    // cluster section
    const auto cluster = root["cluster"];
    if (cluster) {
        config.discovery_backend = ReadString(cluster, "discovery_backend", config.discovery_backend);
        config.node_id = ReadString(cluster, "node_id", config.node_id);
        config.advertise_host = ReadString(cluster, "advertise_host", config.advertise_host);
        config.node_zone = ReadString(cluster, "node_zone", config.node_zone);
        if (cluster["node_weight"]) {
            config.node_weight = static_cast<std::uint32_t>(cluster["node_weight"].as<unsigned int>());
            if (config.node_weight == 0) {
                config.node_weight = 100;
            }
        }
        if (cluster["replication_factor"]) {
            config.replication_factor = cluster["replication_factor"].as<std::size_t>();
        }
        config.data_migration_enabled = ReadBool(cluster, "data_migration_enabled", config.data_migration_enabled);
        config.migration_delete_delay_ms = static_cast<std::uint64_t>(
            ReadSize(cluster, "migration_delete_delay_ms", static_cast<std::size_t>(config.migration_delete_delay_ms)));
        config.migration_batch_size = ReadSize(cluster, "migration_batch_size", config.migration_batch_size);
        config.migration_max_retries = ReadSize(cluster, "migration_max_retries", config.migration_max_retries);
        config.cluster_nodes = ReadString(cluster, "nodes", config.cluster_nodes);
        config.etcd_endpoints = ReadString(cluster, "etcd_endpoints", config.etcd_endpoints);
        config.etcd_prefix = ReadString(cluster, "etcd_prefix", config.etcd_prefix);
        if (cluster["etcd_lease_ttl_s"]) {
            config.etcd_lease_ttl_s = static_cast<std::uint32_t>(cluster["etcd_lease_ttl_s"].as<unsigned int>());
        }
    }

    // security section
    const auto security = root["security"];
    if (security) {
        config.tls_mode = ReadString(security, "tls_mode", config.tls_mode);
    }

    // logging section
    const auto logging = root["logging"];
    if (logging) {
        config.log_level = ReadString(logging, "level", config.log_level);
        config.log_file_path = ReadString(logging, "file_path", config.log_file_path);
        if (logging["file_max_size_mb"]) {
            config.log_file_max_size_mb = logging["file_max_size_mb"].as<std::size_t>();
        }
        if (logging["file_max_files"]) {
            config.log_file_max_files = logging["file_max_files"].as<std::size_t>();
        }
    }

    return config;
}

#endif  // KVAI_HAVE_YAML_CPP

/// Load config from flat dotted-key format (legacy parser, no yaml-cpp needed).
[[maybe_unused]] ServerConfig LoadFlatConfig(const std::string& path) {
    std::ifstream input(path);
    ServerConfig config;
    if (!input.is_open()) {
        return config;
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
    if (const auto iterator = values.find("ai.tokenizer_path"); iterator != values.end()) {
        config.tokenizer_path = iterator->second;
    }
    if (const auto iterator = values.find("ai.max_tokens"); iterator != values.end()) {
        config.ai_max_tokens = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("ai.embedding_dimensions"); iterator != values.end()) {
        config.embedding_dimensions = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("ai.backend"); iterator != values.end()) {
        config.ai_backend = iterator->second;
    }
    if (const auto iterator = values.find("storage.default_collection"); iterator != values.end()) {
        config.default_collection = iterator->second;
    }
    if (const auto iterator = values.find("storage.backend"); iterator != values.end()) {
        config.storage_backend = iterator->second;
    }
    if (const auto iterator = values.find("storage.read_only_mode"); iterator != values.end()) {
        config.read_only_mode = ParseBool(iterator->second);
    }
    if (const auto iterator = values.find("storage.enable_demo_data"); iterator != values.end()) {
        config.enable_demo_data = ParseBool(iterator->second);
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
    if (const auto iterator = values.find("search.backend"); iterator != values.end()) {
        config.search_backend = iterator->second;
    }
    if (const auto iterator = values.find("search.index_path"); iterator != values.end()) {
        config.index_path = iterator->second;
    }
    if (const auto iterator = values.find("search.faiss_nlist"); iterator != values.end()) {
        config.search_faiss_nlist = std::stoi(iterator->second);
    }
    if (const auto iterator = values.find("search.faiss_nprobe"); iterator != values.end()) {
        config.search_faiss_nprobe = std::stoi(iterator->second);
    }
    if (const auto iterator = values.find("cluster.discovery_backend"); iterator != values.end()) {
        config.discovery_backend = iterator->second;
    }
    if (const auto iterator = values.find("cluster.node_id"); iterator != values.end()) {
        config.node_id = iterator->second;
    }
    if (const auto iterator = values.find("cluster.advertise_host"); iterator != values.end()) {
        config.advertise_host = iterator->second;
    }
    if (const auto iterator = values.find("cluster.node_zone"); iterator != values.end()) {
        config.node_zone = iterator->second;
    }
    if (const auto iterator = values.find("cluster.node_weight"); iterator != values.end()) {
        config.node_weight = static_cast<std::uint32_t>(std::stoul(iterator->second));
        if (config.node_weight == 0) {
            config.node_weight = 100;
        }
    }
    if (const auto iterator = values.find("cluster.replication_factor"); iterator != values.end()) {
        config.replication_factor = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("cluster.data_migration_enabled"); iterator != values.end()) {
        config.data_migration_enabled = ParseBool(iterator->second);
    }
    if (const auto iterator = values.find("cluster.migration_delete_delay_ms"); iterator != values.end()) {
        config.migration_delete_delay_ms = static_cast<std::uint64_t>(std::stoull(iterator->second));
    }
    if (const auto iterator = values.find("cluster.migration_batch_size"); iterator != values.end()) {
        config.migration_batch_size = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("cluster.migration_max_retries"); iterator != values.end()) {
        config.migration_max_retries = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("cluster.nodes"); iterator != values.end()) {
        config.cluster_nodes = iterator->second;
    }
    if (const auto iterator = values.find("cluster.etcd_endpoints"); iterator != values.end()) {
        config.etcd_endpoints = iterator->second;
    }
    if (const auto iterator = values.find("cluster.etcd_prefix"); iterator != values.end()) {
        config.etcd_prefix = iterator->second;
    }
    if (const auto iterator = values.find("cluster.etcd_lease_ttl_s"); iterator != values.end()) {
        config.etcd_lease_ttl_s = static_cast<std::uint32_t>(std::stoul(iterator->second));
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
    if (const auto iterator = values.find("logging.level"); iterator != values.end()) {
        config.log_level = iterator->second;
    }
    if (const auto iterator = values.find("logging.file_path"); iterator != values.end()) {
        config.log_file_path = iterator->second;
    }
    if (const auto iterator = values.find("logging.file_max_size_mb"); iterator != values.end()) {
        config.log_file_max_size_mb = std::stoul(iterator->second);
    }
    if (const auto iterator = values.find("logging.file_max_files"); iterator != values.end()) {
        config.log_file_max_files = std::stoul(iterator->second);
    }

    return config;
}

}  // namespace

StatusOr<ServerConfig> ConfigLoader::LoadFromFile(const std::string& path) {
    // First check file exists
    {
        std::ifstream probe(path);
        if (!probe.is_open()) {
            return Status::NotFound("config file not found: " + path);
        }
    }

#if defined(KVAI_HAVE_YAML_CPP)
    try {
        auto root = YAML::LoadFile(path);

        if (!root.IsMap()) {
            return Status::InvalidArgument("config file must be a YAML mapping: " + path);
        }

        // Expand ${ENV} in all scalar values
        ExpandEnvVarsInNode(root);

        if (IsNestedFormat(root)) {
            return LoadNestedConfig(root);
        }

        // Flat format detected — parse using dotted-key extraction from the YAML node
        log::Warn("config", "flat dotted-key YAML format is deprecated, use nested sections", {{"path", path}});
        ServerConfig config;
        std::unordered_map<std::string, std::string> values;

        for (auto iterator = root.begin(); iterator != root.end(); ++iterator) {
            auto key = iterator->first.as<std::string>();
            if (iterator->second.IsScalar()) {
                values[key] = iterator->second.as<std::string>();
            }
        }

        // Reuse the flat parsing logic but from the in-memory map
        if (const auto it = values.find("server.host"); it != values.end()) {
            config.host = it->second;
        }
        if (const auto it = values.find("server.port"); it != values.end()) {
            config.port = static_cast<std::uint16_t>(std::stoul(it->second));
        }
        if (const auto it = values.find("server.worker_threads"); it != values.end()) {
            config.worker_threads = std::stoul(it->second);
        }
        if (const auto it = values.find("server.graceful_shutdown_timeout_ms"); it != values.end()) {
            config.graceful_shutdown_timeout_ms = static_cast<std::uint32_t>(std::stoul(it->second));
        }
        if (const auto it = values.find("search.max_top_k"); it != values.end()) {
            config.max_top_k = std::stoul(it->second);
        }
        if (const auto it = values.find("gateway.rate_limit_per_second"); it != values.end()) {
            config.rate_limit_per_second = std::stoul(it->second);
        }
        if (const auto it = values.find("gateway.require_api_key"); it != values.end()) {
            config.require_api_key = ParseBool(it->second);
        }
        if (const auto it = values.find("gateway.api_key"); it != values.end()) {
            config.api_key = it->second;
        }
        if (const auto it = values.find("ai.timeout_ms"); it != values.end()) {
            config.ai_timeout_ms = static_cast<std::uint32_t>(std::stoul(it->second));
        }
        if (const auto it = values.find("ai.model_path"); it != values.end()) {
            config.model_path = it->second;
        }
        if (const auto it = values.find("ai.tokenizer_path"); it != values.end()) {
            config.tokenizer_path = it->second;
        }
        if (const auto it = values.find("ai.max_tokens"); it != values.end()) {
            config.ai_max_tokens = std::stoul(it->second);
        }
        if (const auto it = values.find("ai.embedding_dimensions"); it != values.end()) {
            config.embedding_dimensions = std::stoul(it->second);
        }
        if (const auto it = values.find("ai.backend"); it != values.end()) {
            config.ai_backend = it->second;
        }
        if (const auto it = values.find("storage.default_collection"); it != values.end()) {
            config.default_collection = it->second;
        }
        if (const auto it = values.find("storage.backend"); it != values.end()) {
            config.storage_backend = it->second;
        }
        if (const auto it = values.find("storage.read_only_mode"); it != values.end()) {
            config.read_only_mode = ParseBool(it->second);
        }
        if (const auto it = values.find("storage.enable_demo_data"); it != values.end()) {
            config.enable_demo_data = ParseBool(it->second);
        }
        if (const auto it = values.find("storage.wal_path"); it != values.end()) {
            config.wal_path = it->second;
        }
        if (const auto it = values.find("storage.snapshot_path"); it != values.end()) {
            config.snapshot_path = it->second;
        }
        if (const auto it = values.find("storage.db_path"); it != values.end()) {
            config.db_path = it->second;
        }
        if (const auto it = values.find("search.backend"); it != values.end()) {
            config.search_backend = it->second;
        }
        if (const auto it = values.find("search.index_path"); it != values.end()) {
            config.index_path = it->second;
        }
        if (const auto it = values.find("cluster.discovery_backend"); it != values.end()) {
            config.discovery_backend = it->second;
        }
        if (const auto it = values.find("cluster.node_id"); it != values.end()) {
            config.node_id = it->second;
        }
        if (const auto it = values.find("cluster.advertise_host"); it != values.end()) {
            config.advertise_host = it->second;
        }
        if (const auto it = values.find("cluster.node_zone"); it != values.end()) {
            config.node_zone = it->second;
        }
        if (const auto it = values.find("cluster.node_weight"); it != values.end()) {
            config.node_weight = static_cast<std::uint32_t>(std::stoul(it->second));
            if (config.node_weight == 0) {
                config.node_weight = 100;
            }
        }
        if (const auto it = values.find("cluster.replication_factor"); it != values.end()) {
            config.replication_factor = std::stoul(it->second);
        }
        if (const auto it = values.find("cluster.data_migration_enabled"); it != values.end()) {
            config.data_migration_enabled = ParseBool(it->second);
        }
        if (const auto it = values.find("cluster.migration_delete_delay_ms"); it != values.end()) {
            config.migration_delete_delay_ms = static_cast<std::uint64_t>(std::stoull(it->second));
        }
        if (const auto it = values.find("cluster.migration_batch_size"); it != values.end()) {
            config.migration_batch_size = std::stoul(it->second);
        }
        if (const auto it = values.find("cluster.migration_max_retries"); it != values.end()) {
            config.migration_max_retries = std::stoul(it->second);
        }
        if (const auto it = values.find("cluster.nodes"); it != values.end()) {
            config.cluster_nodes = it->second;
        }
        if (const auto it = values.find("cluster.etcd_endpoints"); it != values.end()) {
            config.etcd_endpoints = it->second;
        }
        if (const auto it = values.find("cluster.etcd_prefix"); it != values.end()) {
            config.etcd_prefix = it->second;
        }
        if (const auto it = values.find("cluster.etcd_lease_ttl_s"); it != values.end()) {
            config.etcd_lease_ttl_s = static_cast<std::uint32_t>(std::stoul(it->second));
        }
        if (const auto it = values.find("gateway.openapi_path"); it != values.end()) {
            config.openapi_path = it->second;
        }
        if (const auto it = values.find("security.tls_mode"); it != values.end()) {
            config.tls_mode = it->second;
        }
        if (const auto it = values.find("log.level"); it != values.end()) {
            config.log_level = it->second;
        }
        if (const auto it = values.find("logging.level"); it != values.end()) {
            config.log_level = it->second;
        }
        if (const auto it = values.find("logging.file_path"); it != values.end()) {
            config.log_file_path = it->second;
        }
        if (const auto it = values.find("logging.file_max_size_mb"); it != values.end()) {
            config.log_file_max_size_mb = std::stoul(it->second);
        }
        if (const auto it = values.find("logging.file_max_files"); it != values.end()) {
            config.log_file_max_files = std::stoul(it->second);
        }

        return config;

    } catch (const YAML::Exception& error) {
        return Status::Internal(std::string("YAML parse error: ") + error.what());
    }
#else
    // No yaml-cpp available — use the legacy flat-format parser
    auto config = LoadFlatConfig(path);
    return config;
#endif
}

}  // namespace kvai::infra
