#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "infra/status.h"

namespace kvai::infra {

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::size_t worker_threads = 4;
    std::uint32_t graceful_shutdown_timeout_ms = 5000;
    std::size_t max_top_k = 50;
    std::size_t embedding_dimensions = 32;
    std::size_t rate_limit_per_second = 200;
    std::uint32_t ai_timeout_ms = 25;
    std::string default_collection = "documents";
    std::string ai_backend = "deterministic";
    std::string storage_backend = "wal";
    std::string search_backend = "brute_force";
    std::string discovery_backend = "static";
    std::string node_id = "node-local";
    std::size_t replication_factor = 1;
    bool read_only_mode = false;
    bool enable_demo_data = true;
    bool require_api_key = false;
    std::string api_key;
    std::string tls_mode = "disabled";
    std::string cluster_nodes = "node-local@127.0.0.1:8080";
    std::string model_path = "./models/clip.onnx";
    std::string wal_path = "./data/kvai.wal";
    std::string snapshot_path = "./data/kvai.snapshot";
    std::string db_path = "./data/rocksdb";
    std::string index_path = "./data/kvai.index";
    std::string openapi_path = "./docs/openapi.yaml";
    std::string log_level = "info";
};

class ConfigLoader {
public:
    static StatusOr<ServerConfig> LoadFromFile(const std::string& path);
};

}  // namespace kvai::infra