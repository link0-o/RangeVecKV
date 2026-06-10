#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ai/embedding_service.h"
#include "core/kv_store.h"
#include "gateway/data_migration.h"
#include "gateway/interceptors.h"
#include "gateway/semantic_search_service.h"
#include "gateway/vector_index_outbox.h"
#include "infra/cluster_routing.h"
#include "infra/config.h"
#include "infra/etcd_discovery.h"
#include "infra/metrics.h"
#include "infra/status.h"
#include "infra/thread_pool.h"
#include "search/vector_index.h"

namespace kvai::gateway {

class InProcessGatewayServer {
public:
    explicit InProcessGatewayServer(kvai::infra::ServerConfig config);

    kvai::infra::Status Start();
    kvai::infra::Status Stop();

    kvai::infra::StatusOr<SemanticSearchResult> Search(SemanticSearchQuery query);
    kvai::infra::Status UpsertDocument(kvai::core::DocumentRecord record, std::string trace_id);
    kvai::infra::Status DeleteDocument(std::string collection, std::string key, std::string trace_id);
    kvai::infra::Status PutKvRecord(kvai::core::DocumentRecord record, std::string trace_id);
    kvai::infra::StatusOr<kvai::core::DocumentRecord> GetKvRecord(std::string collection, std::string key, std::string trace_id) const;
    kvai::infra::StatusOr<std::vector<kvai::core::DocumentRecord>> RangeKvRecords(std::string collection,
                                                                                   std::string begin_key,
                                                                                   std::string end_key,
                                                                                   std::size_t limit) const;
    kvai::infra::Status DeleteKvRecord(std::string collection, std::string key, std::string trace_id);
    kvai::infra::Status ApplyMigratedRecord(kvai::core::DocumentRecord record, bool semantic, std::string trace_id);
    void TriggerMigrationScan();
    [[nodiscard]] DataMigrationStatus MigrationStatus() const;
    [[nodiscard]] VectorIndexOutboxStatus VectorOutboxStatus() const;
    kvai::infra::Status ReindexDocuments(std::string collection);
    [[nodiscard]] kvai::infra::RouteDecision DescribeRoute(const std::string& collection, const std::string& key) const;
    HealthReport HealthCheck(std::string trace_id) const;
    [[nodiscard]] kvai::infra::MetricsRegistry& MutableMetrics();
    [[nodiscard]] const kvai::infra::MetricsRegistry& Metrics() const;
    [[nodiscard]] std::string OpenApiSpec() const;

private:
    kvai::infra::Status SeedDemoData();

    kvai::infra::ServerConfig config_;
    kvai::infra::ThreadPool thread_pool_;
    std::unique_ptr<kvai::core::KvStore> kv_store_;
    std::unique_ptr<kvai::ai::EmbeddingService> embedding_service_;
    std::unique_ptr<kvai::search::VectorIndex> vector_index_;
    kvai::infra::ConsistentHashRouter router_;
    kvai::infra::MetricsRegistry metrics_;
    FixedWindowRateLimiter rate_limiter_;
    std::unique_ptr<SemanticSearchService> service_;
    std::unique_ptr<kvai::infra::EtcdServiceDiscovery> discovery_;
    std::unique_ptr<DataMigrationManager> migration_manager_;
    std::unique_ptr<VectorIndexOutbox> vector_outbox_;
    bool started_ = false;
};

}  // namespace kvai::gateway
