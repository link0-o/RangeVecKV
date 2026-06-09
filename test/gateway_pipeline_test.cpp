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

    if (!Expect(server.Stop().ok(), "server stop failed")) {
        return 1;
    }

    std::filesystem::remove_all(temp_dir);
    return 0;
}
