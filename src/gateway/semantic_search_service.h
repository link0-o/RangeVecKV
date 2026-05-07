#pragma once

#include <memory>
#include <string>

#include "ai/embedding_service.h"
#include "core/kv_store.h"
#include "gateway/service_types.h"
#include "infra/config.h"
#include "infra/metrics.h"
#include "infra/status.h"
#include "infra/thread_pool.h"
#include "search/vector_index.h"

namespace kvai::gateway {

class SemanticSearchService {
public:
    SemanticSearchService(const kvai::infra::ServerConfig& config,
                          kvai::infra::ThreadPool& thread_pool,
                          kvai::core::KvStore& kv_store,
                          kvai::ai::EmbeddingService& embedding_service,
                          kvai::search::VectorIndex& vector_index,
                          kvai::infra::MetricsRegistry& metrics);

    kvai::infra::StatusOr<SemanticSearchResult> Search(const SemanticSearchQuery& query);
    HealthReport HealthCheck(const std::string& trace_id) const;

private:
    kvai::infra::StatusOr<SemanticSearchResult> KeywordFallback(const SemanticSearchQuery& query, const std::string& trace_id, const std::string& reason);
    static std::string BuildSnippet(const std::string& body, const std::string& query);

    const kvai::infra::ServerConfig& config_;
    kvai::infra::ThreadPool& thread_pool_;
    kvai::core::KvStore& kv_store_;
    kvai::ai::EmbeddingService& embedding_service_;
    kvai::search::VectorIndex& vector_index_;
    kvai::infra::MetricsRegistry& metrics_;
};

}  // namespace kvai::gateway