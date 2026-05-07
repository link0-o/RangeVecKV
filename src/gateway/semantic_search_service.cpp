#include "gateway/semantic_search_service.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>

namespace kvai::gateway {

namespace {

std::vector<kvai::search::SearchFilter> ConvertFilters(const std::vector<SearchFilter>& filters) {
    std::vector<kvai::search::SearchFilter> converted;
    converted.reserve(filters.size());
    for (const auto& filter : filters) {
        converted.push_back(kvai::search::SearchFilter{filter.field, filter.value});
    }
    return converted;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

}  // namespace

SemanticSearchService::SemanticSearchService(const kvai::infra::ServerConfig& config,
                                             kvai::infra::ThreadPool& thread_pool,
                                             kvai::core::KvStore& kv_store,
                                             kvai::ai::EmbeddingService& embedding_service,
                                             kvai::search::VectorIndex& vector_index,
                                             kvai::infra::MetricsRegistry& metrics)
    : config_(config), thread_pool_(thread_pool), kv_store_(kv_store), embedding_service_(embedding_service), vector_index_(vector_index), metrics_(metrics) {}

kvai::infra::StatusOr<SemanticSearchResult> SemanticSearchService::Search(const SemanticSearchQuery& query) {
    if (query.query.empty()) {
        return kvai::infra::Status::InvalidArgument("query cannot be empty");
    }

    const auto effective_collection = query.collection.empty() ? config_.default_collection : query.collection;
    const auto effective_top_k = std::min<std::size_t>(std::max<std::size_t>(1, query.top_k), config_.max_top_k);

    auto embedding_future = thread_pool_.Submit([this, payload = query.query]() { return embedding_service_.EmbedText(payload); });
    if (embedding_future.wait_for(std::chrono::milliseconds(config_.ai_timeout_ms)) != std::future_status::ready) {
        return KeywordFallback(query, query.trace_id, "embedding timeout triggered keyword fallback");
    }

    auto embedding = embedding_future.get();
    if (!embedding.ok()) {
        return KeywordFallback(query, query.trace_id, embedding.status().ToString());
    }

    auto references = vector_index_.Search(effective_collection, embedding.value().values, ConvertFilters(query.filters), effective_top_k);
    if (!references.ok()) {
        return references.status();
    }

    auto documents = kv_store_.MultiGet(references.value());
    if (!documents.ok()) {
        return documents.status();
    }

    SemanticSearchResult result;
    result.trace_id = query.trace_id;
    result.degraded = embedding.value().degraded;
    result.message = "semantic search completed";

    for (std::size_t index = 0; index < documents.value().size(); ++index) {
        const auto& record = documents.value()[index];
        const auto score = index < references.value().size() ? references.value()[index].score : 0.0;
        result.hits.push_back(SearchHit{record.key, record.title, BuildSnippet(record.body, query.query), score, record.metadata});
    }

    return result;
}

HealthReport SemanticSearchService::HealthCheck(const std::string& trace_id) const {
    HealthReport report;
    report.trace_id = trace_id;
    report.status = "SERVING";
    report.version = "0.1.0";
    report.details.emplace("ai_backend", config_.ai_backend);
    report.details.emplace("storage_backend", config_.storage_backend);
    report.details.emplace("search_backend", config_.search_backend);
    report.details.emplace("search_requests_total", std::to_string(metrics_.search_requests()));
    report.details.emplace("search_failures_total", std::to_string(metrics_.search_failures()));
    report.details.emplace("degraded_search_total", std::to_string(metrics_.degraded_searches()));
    report.details.emplace("http_requests_total", std::to_string(metrics_.http_requests()));
    if (config_.ai_backend == "deterministic") {
        report.warnings.push_back("deterministic embedding fallback enabled; configure ONNX adapter before production rollout");
    }
    if (config_.search_backend == "brute_force") {
        report.warnings.push_back("brute-force vector index enabled; configure FAISS adapter before production rollout");
    }
    if (config_.storage_backend == "wal") {
        report.warnings.push_back("local WAL store enabled; configure RocksDB adapter before production rollout");
    }
    return report;
}

kvai::infra::StatusOr<SemanticSearchResult> SemanticSearchService::KeywordFallback(const SemanticSearchQuery& query,
                                                                                   const std::string& trace_id,
                                                                                   const std::string& reason) {
    const auto effective_collection = query.collection.empty() ? config_.default_collection : query.collection;
    auto all_documents = kv_store_.Range(effective_collection, "", "", 1000);
    if (!all_documents.ok()) {
        return all_documents.status();
    }

    const auto lowered_query = ToLower(query.query);
    std::vector<std::pair<double, kvai::core::DocumentRecord>> ranked;

    for (const auto& record : all_documents.value()) {
        double score = 0.0;
        const auto searchable = ToLower(record.title + " " + record.body);
        if (searchable.find(lowered_query) != std::string::npos) {
            score += 5.0;
        }
        for (const auto& token : query.filters) {
            const auto iterator = record.metadata.find(token.field);
            if (iterator != record.metadata.end() && iterator->second == token.value) {
                score += 1.0;
            }
        }
        if (score > 0.0) {
            ranked.emplace_back(score, record);
        }
    }

    std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    if (ranked.size() > query.top_k) {
        ranked.resize(query.top_k);
    }

    SemanticSearchResult result;
    result.trace_id = trace_id;
    result.degraded = true;
    result.message = "keyword fallback: " + reason;

    for (const auto& [score, record] : ranked) {
        result.hits.push_back(SearchHit{record.key, record.title, BuildSnippet(record.body, query.query), score, record.metadata});
    }

    return result;
}

std::string SemanticSearchService::BuildSnippet(const std::string& body, const std::string& query) {
    if (body.empty()) {
        return {};
    }

    const auto position = ToLower(body).find(ToLower(query));
    if (position == std::string::npos) {
        return body.substr(0, std::min<std::size_t>(body.size(), 120));
    }

    const auto begin = position > 40 ? position - 40 : 0;
    return body.substr(begin, std::min<std::size_t>(body.size() - begin, 120));
}

}  // namespace kvai::gateway