#pragma once

#include <memory>
#include <string>

#include "ai/embedding_service.h"
#include "core/kv_store.h"
#include "gateway/interceptors.h"
#include "gateway/semantic_search_service.h"
#include "infra/cluster_routing.h"
#include "infra/config.h"
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
    bool started_ = false;
};

}  // namespace kvai::gateway