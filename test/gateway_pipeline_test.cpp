#include <filesystem>
#include <iostream>

#include "gateway/server.h"

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
    const auto temp_dir = std::filesystem::temp_directory_path() / "rangeveckv-gateway-test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    kvai::infra::ServerConfig config;
    config.wal_path = (temp_dir / "gateway.wal").string();
    config.snapshot_path = (temp_dir / "gateway.snapshot").string();
    config.db_path = (temp_dir / "rocksdb").string();
    config.index_path = (temp_dir / "gateway.index").string();
    config.vector_index_outbox_path = (temp_dir / "gateway.outbox").string();
    config.migration_task_wal_path = (temp_dir / "migration_tasks.json").string();
    config.default_collection = "documents";
    config.embedding_dimensions = 32;
    config.worker_threads = 2;
    config.rate_limit_per_second = 100;

    kvai::gateway::InProcessGatewayServer server(config);
    if (!Expect(server.Start().ok(), "server start failed")) {
        return 1;
    }

    kvai::gateway::SemanticSearchQuery query;
    query.collection = "documents";
    query.query = "trace identifiers and gateway health";
    query.top_k = 3;

    auto result = server.Search(query);
    if (!Expect(result.ok(), "search failed")) {
        return 1;
    }
    if (!Expect(!result.value().hits.empty(), "search returned no hits")) {
        return 1;
    }
    if (!Expect(!result.value().trace_id.empty(), "trace id missing")) {
        return 1;
    }

    auto report = server.HealthCheck("");
    if (!Expect(report.status == "SERVING", "health status mismatch")) {
        return 1;
    }
    if (!Expect(report.details.find("search_backend") != report.details.end(), "health details missing backend summary")) {
        return 1;
    }
    if (!Expect(report.details.find("etcd_discovery_state") != report.details.end(), "health details missing discovery state")) {
        return 1;
    }
    if (!Expect(report.details.find("remote_forwarding_enabled") != report.details.end(), "health details missing forwarding boundary")) {
        return 1;
    }
    if (!Expect(report.details.find("data_migration_state") != report.details.end(), "health details missing migration state")) {
        return 1;
    }
    if (!Expect(report.details.find("vector_index_outbox_state") != report.details.end(), "health details missing vector outbox state")) {
        return 1;
    }

    kvai::core::DocumentRecord record{"documents", "doc-999", "Router Aware Write", "Cluster aware document write path", {{"domain", "gateway"}}};
    if (!Expect(server.UpsertDocument(record, "").ok(), "upsert document failed")) {
        return 1;
    }
    if (!Expect(server.ReindexDocuments("documents").ok(), "reindex documents failed")) {
        return 1;
    }

    auto route = server.DescribeRoute("documents", "doc-999");
    if (!Expect(route.has_primary, "route decision missing primary")) {
        return 1;
    }
    if (!Expect(route.local_owner, "single-node route should resolve locally")) {
        return 1;
    }

    if (!Expect(server.DeleteDocument("documents", "doc-999", "").ok(), "delete document failed")) {
        return 1;
    }

    kvai::core::DocumentRecord kv_record{"kv", "user:1", "", "plain kv payload", {{"kind", "kv-only"}}};
    if (!Expect(server.PutKvRecord(kv_record, "").ok(), "kv put failed")) {
        return 1;
    }
    kvai::core::DocumentRecord kv_batch_a{"kv", "batch:user:1", "", "batch payload 1", {{"kind", "kv-only"}}};
    kvai::core::DocumentRecord kv_batch_b{"kv", "batch:user:2", "", "batch payload 2", {{"kind", "kv-only"}}};
    if (!Expect(server.PutKvRecords({kv_batch_a, kv_batch_b}, "").ok(), "kv batch put failed")) {
        return 1;
    }
    auto batch_stored = server.GetKvRecord("kv", "batch:user:2", "");
    if (!Expect(batch_stored.ok() && batch_stored.value().body == "batch payload 2", "kv batch get returned wrong value")) {
        return 1;
    }
    auto stored = server.GetKvRecord("kv", "user:1", "");
    if (!Expect(stored.ok(), "kv get failed")) {
        return 1;
    }
    if (!Expect(stored.value().body == "plain kv payload", "kv get returned wrong value")) {
        return 1;
    }
    auto range = server.RangeKvRecords("kv", "", "", 10);
    if (!Expect(range.ok(), "kv range failed")) {
        return 1;
    }
    if (!Expect(!range.value().empty(), "kv range returned no records")) {
        return 1;
    }
    if (!Expect(server.DeleteKvRecord("kv", "user:1", "").ok(), "kv delete failed")) {
        return 1;
    }
    auto deleted = server.GetKvRecord("kv", "user:1", "");
    if (!Expect(!deleted.ok(), "kv get should fail after delete")) {
        return 1;
    }

    kvai::core::DocumentRecord migrated_kv{"kv", "migrated:1", "", "migrated payload", {{"kind", "migration"}}};
    if (!Expect(server.ApplyMigratedRecord(migrated_kv, false, "").ok(), "migrated kv apply failed")) {
        return 1;
    }
    auto migrated_stored = server.GetKvRecord("kv", "migrated:1", "");
    if (!Expect(migrated_stored.ok(), "migrated kv missing")) {
        return 1;
    }

    kvai::core::DocumentRecord migrated_doc{"documents", "migrated-doc", "Migrated Semantic", "semantic migration payload", {{"domain", "migration"}}};
    if (!Expect(server.ApplyMigratedRecord(migrated_doc, true, "").ok(), "migrated semantic apply failed")) {
        return 1;
    }
    kvai::gateway::SemanticSearchQuery migrated_query;
    migrated_query.collection = "documents";
    migrated_query.query = "semantic migration payload";
    migrated_query.top_k = 3;
    auto migrated_search = server.Search(migrated_query);
    if (!Expect(migrated_search.ok(), "migrated semantic search failed")) {
        return 1;
    }
    if (!Expect(!migrated_search.value().hits.empty(), "migrated semantic search returned no hits")) {
        return 1;
    }

    if (!Expect(server.Stop().ok(), "server stop failed")) {
        return 1;
    }

    kvai::infra::ServerConfig remote_config;
    remote_config.wal_path = (temp_dir / "remote.wal").string();
    remote_config.snapshot_path = (temp_dir / "remote.snapshot").string();
    remote_config.db_path = (temp_dir / "remote-rocksdb").string();
    remote_config.index_path = (temp_dir / "remote.index").string();
    remote_config.vector_index_outbox_path = (temp_dir / "remote.outbox").string();
    remote_config.migration_task_wal_path = (temp_dir / "remote-migration.json").string();
    remote_config.enable_demo_data = false;
    remote_config.node_id = "local-node";
    remote_config.cluster_nodes = "remote-node@10.0.0.2:9090";
    kvai::gateway::InProcessGatewayServer remote_server(remote_config);
    if (!Expect(remote_server.Start().ok(), "remote-owner server start failed")) {
        return 1;
    }
    kvai::core::DocumentRecord remote_record{"kv", "remote-key", "", "payload", {}};
    auto remote_status = remote_server.PutKvRecord(remote_record, "");
    if (!Expect(!remote_status.ok(), "remote owner write should be rejected")) {
        return 1;
    }
    if (!Expect(remote_status.message().find("endpoint=10.0.0.2:9090") != std::string::npos,
                "remote owner error should include endpoint")) {
        return 1;
    }
    kvai::core::DocumentRecord remote_batch_record{"kv", "remote-batch-key", "", "payload", {}};
    auto remote_batch_status = remote_server.PutKvRecords({remote_batch_record}, "");
    if (!Expect(!remote_batch_status.ok(), "remote owner batch write should be rejected")) {
        return 1;
    }
    if (!Expect(remote_batch_status.message().find("endpoint=10.0.0.2:9090") != std::string::npos,
                "remote owner batch error should include endpoint")) {
        return 1;
    }
    if (!Expect(remote_server.Stop().ok(), "remote-owner server stop failed")) {
        return 1;
    }

    std::filesystem::remove_all(temp_dir);
    return 0;
}
