#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infra/config.h"

namespace {

bool Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "rangeveckv-config-test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    int failures = 0;

    // --- Test 1: Flat dotted-key format (always available) ---
    {
        const auto config_path = temp_dir / "flat.yaml";
        {
            std::ofstream output(config_path);
            output << "gateway.require_api_key: true\n";
            output << "gateway.api_key: ${KVAI_TEST_API_KEY}\n";
            output << "ai.tokenizer_path: /models/vocab.txt\n";
            output << "ai.max_tokens: 128\n";
            output << "cluster.nodes: ${KVAI_TEST_CLUSTER}\n";
            output << "storage.enable_demo_data: false\n";
            output << "logging.level: debug\n";
            output << "logging.file_path: /tmp/kvai.log\n";
            output << "cluster.advertise_host: node-a\n";
            output << "cluster.node_weight: 150\n";
            output << "cluster.node_zone: zone-a\n";
            output << "cluster.data_migration_enabled: true\n";
            output << "cluster.migration_delete_delay_ms: 42\n";
            output << "cluster.migration_batch_size: 7\n";
            output << "cluster.migration_max_retries: 3\n";
            output << "cluster.etcd_endpoints: http://etcd:2379\n";
        }

        ::setenv("KVAI_TEST_API_KEY", "integration-secret", 1);
        ::setenv("KVAI_TEST_CLUSTER", "node-a@127.0.0.1:8080", 1);

        auto config = kvai::infra::ConfigLoader::LoadFromFile(config_path.string());
        if (!Expect(config.ok(), "flat: config loading failed")) {
            failures += 10;
        } else {
            if (!Expect(config.value().require_api_key, "flat: api key flag not parsed")) ++failures;
            if (!Expect(config.value().api_key == "integration-secret", "flat: api key env expansion failed")) ++failures;
            if (!Expect(config.value().tokenizer_path == "/models/vocab.txt", "flat: tokenizer_path not parsed")) ++failures;
            if (!Expect(config.value().ai_max_tokens == 128, "flat: max_tokens not parsed")) ++failures;
            if (!Expect(config.value().cluster_nodes == "node-a@127.0.0.1:8080", "flat: cluster env expansion failed")) ++failures;
            if (!Expect(!config.value().enable_demo_data, "flat: boolean false parsing failed")) ++failures;
            if (!Expect(config.value().log_level == "debug", "flat: logging.level not parsed")) ++failures;
            if (!Expect(config.value().log_file_path == "/tmp/kvai.log", "flat: logging.file_path not parsed")) ++failures;
            if (!Expect(config.value().advertise_host == "node-a", "flat: advertise_host not parsed")) ++failures;
            if (!Expect(config.value().node_weight == 150, "flat: node_weight not parsed")) ++failures;
            if (!Expect(config.value().node_zone == "zone-a", "flat: node_zone not parsed")) ++failures;
            if (!Expect(config.value().data_migration_enabled, "flat: data_migration_enabled not parsed")) ++failures;
            if (!Expect(config.value().migration_delete_delay_ms == 42, "flat: migration_delete_delay_ms not parsed")) ++failures;
            if (!Expect(config.value().migration_batch_size == 7, "flat: migration_batch_size not parsed")) ++failures;
            if (!Expect(config.value().migration_max_retries == 3, "flat: migration_max_retries not parsed")) ++failures;
            if (!Expect(config.value().etcd_endpoints == "http://etcd:2379", "flat: etcd_endpoints not parsed")) ++failures;
        }
    }

#if defined(KVAI_HAVE_YAML_CPP)
    // --- Test 2: Nested YAML format (requires yaml-cpp) ---
    {
        const auto config_path = temp_dir / "nested.yaml";
        {
            std::ofstream output(config_path);
            output << "server:\n";
            output << "  host: 0.0.0.0\n";
            output << "  port: 9090\n";
            output << "  worker_threads: 8\n";
            output << "gateway:\n";
            output << "  require_api_key: true\n";
            output << "  api_key: ${KVAI_TEST_API_KEY}\n";
            output << "  rate_limit_per_second: 500\n";
            output << "ai:\n";
            output << "  backend: deterministic\n";
            output << "  timeout_ms: 50\n";
            output << "  tokenizer_path: /models/vocab.txt\n";
            output << "  max_tokens: 128\n";
            output << "  embedding_dimensions: 64\n";
            output << "search:\n";
            output << "  max_top_k: 30\n";
            output << "  backend: brute_force\n";
            output << "storage:\n";
            output << "  backend: wal\n";
            output << "  enable_demo_data: false\n";
            output << "  read_only_mode: true\n";
            output << "cluster:\n";
            output << "  node_id: test-node\n";
            output << "  advertise_host: test-node\n";
            output << "  node_weight: 200\n";
            output << "  node_zone: zone-b\n";
            output << "  data_migration_enabled: true\n";
            output << "  migration_delete_delay_ms: 123\n";
            output << "  migration_batch_size: 9\n";
            output << "  migration_max_retries: 4\n";
            output << "  nodes: ${KVAI_TEST_CLUSTER}\n";
            output << "  etcd_endpoints: http://etcd:2379\n";
            output << "  etcd_lease_ttl_s: 20\n";
            output << "security:\n";
            output << "  tls_mode: enabled\n";
            output << "logging:\n";
            output << "  level: debug\n";
            output << "  file_path: /tmp/kvai.log\n";
            output << "  file_max_size_mb: 5\n";
            output << "  file_max_files: 2\n";
        }

        ::setenv("KVAI_TEST_API_KEY", "nested-secret", 1);
        ::setenv("KVAI_TEST_CLUSTER", "node-b@10.0.0.1:9090", 1);

        auto config = kvai::infra::ConfigLoader::LoadFromFile(config_path.string());
        if (!Expect(config.ok(), "nested: config loading failed")) {
            failures += 19;
        } else {
            if (!Expect(config.value().host == "0.0.0.0", "nested: host not parsed")) ++failures;
            if (!Expect(config.value().port == 9090, "nested: port not parsed")) ++failures;
            if (!Expect(config.value().worker_threads == 8, "nested: worker_threads not parsed")) ++failures;
            if (!Expect(config.value().require_api_key, "nested: api key flag not parsed")) ++failures;
            if (!Expect(config.value().api_key == "nested-secret", "nested: api key env expansion failed")) ++failures;
            if (!Expect(config.value().rate_limit_per_second == 500, "nested: rate limit not parsed")) ++failures;
            if (!Expect(config.value().ai_timeout_ms == 50, "nested: ai timeout not parsed")) ++failures;
            if (!Expect(config.value().tokenizer_path == "/models/vocab.txt", "nested: tokenizer path not parsed")) ++failures;
            if (!Expect(config.value().ai_max_tokens == 128, "nested: max tokens not parsed")) ++failures;
            if (!Expect(config.value().embedding_dimensions == 64, "nested: embedding dimensions not parsed")) ++failures;
            if (!Expect(config.value().max_top_k == 30, "nested: max_top_k not parsed")) ++failures;
            if (!Expect(config.value().cluster_nodes == "node-b@10.0.0.1:9090", "nested: cluster env expansion failed")) ++failures;
            if (!Expect(config.value().advertise_host == "test-node", "nested: advertise_host not parsed")) ++failures;
            if (!Expect(config.value().node_weight == 200, "nested: node_weight not parsed")) ++failures;
            if (!Expect(config.value().node_zone == "zone-b", "nested: node_zone not parsed")) ++failures;
            if (!Expect(config.value().data_migration_enabled, "nested: data_migration_enabled not parsed")) ++failures;
            if (!Expect(config.value().migration_delete_delay_ms == 123, "nested: migration_delete_delay_ms not parsed")) ++failures;
            if (!Expect(config.value().migration_batch_size == 9, "nested: migration_batch_size not parsed")) ++failures;
            if (!Expect(config.value().migration_max_retries == 4, "nested: migration_max_retries not parsed")) ++failures;
            if (!Expect(config.value().etcd_endpoints == "http://etcd:2379", "nested: etcd endpoints not parsed")) ++failures;
            if (!Expect(config.value().etcd_lease_ttl_s == 20, "nested: etcd lease ttl not parsed")) ++failures;
            if (!Expect(config.value().tls_mode == "enabled", "nested: tls_mode not parsed")) ++failures;
            if (!Expect(config.value().log_level == "debug", "nested: log level not parsed")) ++failures;
            if (!Expect(config.value().log_file_path == "/tmp/kvai.log", "nested: log file path not parsed")) ++failures;
            if (!Expect(config.value().read_only_mode, "nested: read_only_mode not parsed")) ++failures;
        }
    }

    // --- Test 3: Flat format detected by yaml-cpp should still work ---
    {
        const auto config_path = temp_dir / "flat-via-yamlcpp.yaml";
        {
            std::ofstream output(config_path);
            output << "gateway.require_api_key: true\n";
            output << "gateway.api_key: ${KVAI_TEST_API_KEY}\n";
            output << "storage.enable_demo_data: false\n";
        }

        ::setenv("KVAI_TEST_API_KEY", "flat-yamlcpp-secret", 1);

        auto config = kvai::infra::ConfigLoader::LoadFromFile(config_path.string());
        if (!Expect(config.ok(), "flat-via-yamlcpp: config loading failed")) {
            failures += 3;
        } else {
            if (!Expect(config.value().require_api_key, "flat-via-yamlcpp: api key flag not parsed")) ++failures;
            if (!Expect(config.value().api_key == "flat-yamlcpp-secret", "flat-via-yamlcpp: env expansion failed")) ++failures;
            if (!Expect(!config.value().enable_demo_data, "flat-via-yamlcpp: boolean parsing failed")) ++failures;
        }
    }
#endif

    // --- Test 4: Missing config file ---
    {
        auto config = kvai::infra::ConfigLoader::LoadFromFile("/nonexistent/path.yaml");
        if (!Expect(!config.ok(), "missing file should return error")) ++failures;
    }

    std::filesystem::remove_all(temp_dir);
    return failures > 0 ? 1 : 0;
}
