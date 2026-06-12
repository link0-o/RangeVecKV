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
    std::size_t kv_batch_max_records = 1000;
    std::uint32_t ai_timeout_ms = 25;
    std::string default_collection = "documents";
    std::string ai_backend = "auto";
    std::string storage_backend = "auto";
    std::string search_backend = "auto";
    std::string discovery_backend = "static";
    std::string node_id = "node-local";
    std::string advertise_host;
    std::uint32_t node_weight = 100;
    std::string node_zone = "default";
    std::size_t replication_factor = 1;
    bool data_migration_enabled = false;
    std::uint64_t migration_delete_delay_ms = 300000;
    std::size_t migration_batch_size = 100;
    std::size_t migration_max_retries = 5;
    std::string migration_task_wal_path = "./data/migration_tasks.wal";
    std::size_t cluster_slot_count = 4096;
    bool read_only_mode = false;
    bool enable_demo_data = true;
    std::size_t rocksdb_max_background_jobs = 8;
    std::size_t rocksdb_write_buffer_size_mb = 128;
    std::size_t rocksdb_max_write_buffer_number = 6;
    std::size_t rocksdb_target_file_size_mb = 128;
    std::uint64_t rocksdb_bytes_per_sync = 1048576;
    std::uint64_t rocksdb_wal_bytes_per_sync = 1048576;
    bool rocksdb_enable_pipelined_write = true;
    bool require_api_key = false;
    std::string api_key;
    std::string tls_mode = "disabled";
    std::string cluster_nodes = "node-local@127.0.0.1:8080";
    std::string model_path = "./models/clip.onnx";
    std::string tokenizer_path;
    std::size_t ai_max_tokens = 256;
    std::string wal_path = "./data/kvai.wal";
    std::string snapshot_path = "./data/kvai.snapshot";
    std::string db_path = "./data/rocksdb";
    std::string index_path = "./data/kvai.index";
    std::string vector_index_outbox_path = "./data/vector_index_outbox.wal";
    int search_faiss_nlist = 0;
    int search_faiss_nprobe = 0;
    std::string openapi_path = "./docs/openapi.yaml";
    std::string log_level = "info";
    std::string log_file_path;
    std::size_t log_file_max_size_mb = 10;
    std::size_t log_file_max_files = 3;
    std::string etcd_endpoints = "http://127.0.0.1:2379";
    std::string etcd_prefix = "/rangeveckv/nodes/";
    std::uint32_t etcd_lease_ttl_s = 10;
};

class ConfigLoader {
public:
    static StatusOr<ServerConfig> LoadFromFile(const std::string& path);
};

}  // namespace kvai::infra
