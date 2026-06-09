#include "gateway/server.h"

#include <chrono>
#include <fstream>

#include "infra/logging.h"

namespace kvai::gateway {

namespace {

std::string DefaultOpenApiSpec() {
    return "openapi: 3.0.3\n"
           "info:\n"
           "  title: RangeVecKV Gateway\n"
           "  version: 0.1.0\n"
           "paths:\n"
           "  /healthz:\n"
           "    get:\n"
           "      summary: Health probe\n"
           "  /metrics:\n"
           "    get:\n"
           "      summary: Prometheus metrics\n"
           "  /v1/search:\n"
           "    get:\n"
           "      summary: Semantic search\n"
           "    post:\n"
           "      summary: Semantic search\n";
}

std::string ImageReferenceForRecord(const kvai::core::DocumentRecord& record) {
    for (const auto* key : {"image_path", "image_reference", "image_uri"}) {
        const auto iterator = record.metadata.find(key);
        if (iterator != record.metadata.end() && !iterator->second.empty()) {
            return iterator->second;
        }
    }
    return {};
}

}  // namespace

InProcessGatewayServer::InProcessGatewayServer(kvai::infra::ServerConfig config)
    : config_(std::move(config)),
      thread_pool_(config_.worker_threads),
            router_(config_.node_id),
      rate_limiter_(config_.rate_limit_per_second) {}

kvai::infra::Status InProcessGatewayServer::Start() {
    if (started_) {
        return kvai::infra::Status::Ok();
    }

    auto kv_store = kvai::core::CreateKvStore(config_);
    if (!kv_store.ok()) {
        return kv_store.status();
    }

    auto embedding_service = kvai::ai::CreateEmbeddingService(config_);
    if (!embedding_service.ok()) {
        return embedding_service.status();
    }

    auto vector_index = kvai::search::CreateVectorIndex(config_);
    if (!vector_index.ok()) {
        return vector_index.status();
    }

    kv_store_ = kv_store.ConsumeValue();
    embedding_service_ = embedding_service.ConsumeValue();
    vector_index_ = vector_index.ConsumeValue();

    auto nodes = kvai::infra::ParseStaticClusterNodes(config_.cluster_nodes);
    if (!nodes.ok()) {
        return nodes.status();
    }
    router_.Rebuild(nodes.value());

    // Start etcd discovery if configured
    if (config_.discovery_backend == "etcd") {
        kvai::infra::ClusterNode local_node;
        local_node.id = config_.node_id;
        local_node.host = config_.advertise_host.empty() ? config_.host : config_.advertise_host;
        local_node.port = config_.port;
        local_node.healthy = true;

        discovery_ = std::make_unique<kvai::infra::EtcdServiceDiscovery>(
            config_.etcd_endpoints, config_.etcd_prefix, local_node, config_.etcd_lease_ttl_s);
        auto discovery_status = discovery_->Start(&router_);
        if (!discovery_status.ok()) {
            kvai::infra::log::Warn("gateway", "etcd discovery failed, falling back to static routing",
                                   {{"error", discovery_status.ToString()}});
            discovery_.reset();
        }
    }

    service_ = std::make_unique<SemanticSearchService>(config_, thread_pool_, *kv_store_, *embedding_service_, *vector_index_, metrics_);
    auto status = SeedDemoData();
    if (!status.ok()) {
        return status;
    }

    started_ = true;
    kvai::infra::log::Info("gateway", "server started",
                           {{"host", config_.host}, {"port", std::to_string(config_.port)}});
    return kvai::infra::Status::Ok();
}

kvai::infra::Status InProcessGatewayServer::Stop() {
    if (!started_) {
        return kvai::infra::Status::Ok();
    }

    if (discovery_) {
        discovery_->Stop();
        discovery_.reset();
    }

    auto status = kv_store_->FlushSnapshot();
    if (!status.ok()) {
        return status;
    }
    status = vector_index_->FlushSnapshot();
    if (!status.ok()) {
        return status;
    }

    started_ = false;
    return kvai::infra::Status::Ok();
}

kvai::infra::StatusOr<SemanticSearchResult> InProcessGatewayServer::Search(SemanticSearchQuery query) {
    if (!started_) {
        return kvai::infra::Status::Unavailable("server not started");
    }
    if (!rate_limiter_.Allow()) {
        metrics_.IncrementSearchFailures();
        return kvai::infra::Status::Unavailable("rate limit exceeded");
    }

    metrics_.IncrementSearchRequests();
    query.trace_id = TraceInjector::EnsureTraceId(query.trace_id);
    const auto started = std::chrono::steady_clock::now();
    auto result = service_->Search(query);
    metrics_.ObserveSearchLatency(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started));
    if (!result.ok()) {
        metrics_.IncrementSearchFailures();
        return result;
    }
    if (result.value().degraded) {
        metrics_.IncrementDegradedSearches();
    }
    return result;
}

kvai::infra::Status InProcessGatewayServer::UpsertDocument(kvai::core::DocumentRecord record, std::string trace_id) {
    if (!started_) {
        return kvai::infra::Status::Unavailable("server not started");
    }
    if (config_.read_only_mode) {
        return kvai::infra::Status::Unavailable("storage is in read-only mode");
    }
    record.collection = record.collection.empty() ? config_.default_collection : record.collection;
    if (record.key.empty()) {
        return kvai::infra::Status::InvalidArgument("document key cannot be empty");
    }

    const auto trace = TraceInjector::EnsureTraceId(trace_id);
    const auto route = DescribeRoute(record.collection, record.key);
    if (route.has_primary && !route.local_owner) {
        return kvai::infra::Status::Unavailable("document owned by remote node: " + route.primary.id + " trace_id=" + trace);
    }

    const auto image_reference = ImageReferenceForRecord(record);
    auto embedding = image_reference.empty()
                         ? embedding_service_->EmbedText(record.title + " " + record.body)
                         : embedding_service_->EmbedImage(image_reference);
    if (!embedding.ok()) {
        return embedding.status();
    }

    auto status = kv_store_->Put(record);
    if (!status.ok()) {
        return status;
    }
    return vector_index_->Upsert(record, embedding.value().values);
}

kvai::infra::Status InProcessGatewayServer::DeleteDocument(std::string collection, std::string key, std::string trace_id) {
    if (!started_) {
        return kvai::infra::Status::Unavailable("server not started");
    }
    if (config_.read_only_mode) {
        return kvai::infra::Status::Unavailable("storage is in read-only mode");
    }
    collection = collection.empty() ? config_.default_collection : collection;
    if (key.empty()) {
        return kvai::infra::Status::InvalidArgument("document key cannot be empty");
    }

    const auto trace = TraceInjector::EnsureTraceId(trace_id);
    const auto route = DescribeRoute(collection, key);
    if (route.has_primary && !route.local_owner) {
        return kvai::infra::Status::Unavailable("document owned by remote node: " + route.primary.id + " trace_id=" + trace);
    }

    auto status = kv_store_->Delete(collection, key);
    if (!status.ok()) {
        return status;
    }
    return vector_index_->Remove(collection, key);
}

kvai::infra::Status InProcessGatewayServer::PutKvRecord(kvai::core::DocumentRecord record, std::string trace_id) {
    if (!started_) {
        return kvai::infra::Status::Unavailable("server not started");
    }
    if (config_.read_only_mode) {
        return kvai::infra::Status::Unavailable("storage is in read-only mode");
    }
    record.collection = record.collection.empty() ? config_.default_collection : record.collection;
    if (record.key.empty()) {
        return kvai::infra::Status::InvalidArgument("kv key cannot be empty");
    }

    const auto trace = TraceInjector::EnsureTraceId(trace_id);
    const auto route = DescribeRoute(record.collection, record.key);
    if (route.has_primary && !route.local_owner) {
        return kvai::infra::Status::Unavailable("kv record owned by remote node: " + route.primary.id + " trace_id=" + trace);
    }

    return kv_store_->Put(record);
}

kvai::infra::StatusOr<kvai::core::DocumentRecord> InProcessGatewayServer::GetKvRecord(std::string collection,
                                                                                       std::string key,
                                                                                       std::string trace_id) const {
    if (!started_) {
        return kvai::infra::Status::Unavailable("server not started");
    }
    collection = collection.empty() ? config_.default_collection : std::move(collection);
    if (key.empty()) {
        return kvai::infra::Status::InvalidArgument("kv key cannot be empty");
    }

    const auto trace = TraceInjector::EnsureTraceId(trace_id);
    const auto route = DescribeRoute(collection, key);
    if (route.has_primary && !route.local_owner) {
        return kvai::infra::Status::Unavailable("kv record owned by remote node: " + route.primary.id + " trace_id=" + trace);
    }

    return kv_store_->Get(collection, key);
}

kvai::infra::StatusOr<std::vector<kvai::core::DocumentRecord>> InProcessGatewayServer::RangeKvRecords(std::string collection,
                                                                                                       std::string begin_key,
                                                                                                       std::string end_key,
                                                                                                       std::size_t limit) const {
    if (!started_) {
        return kvai::infra::Status::Unavailable("server not started");
    }
    collection = collection.empty() ? config_.default_collection : std::move(collection);
    limit = std::min<std::size_t>(std::max<std::size_t>(1, limit), 1000);
    return kv_store_->Range(collection, begin_key, end_key, limit);
}

kvai::infra::Status InProcessGatewayServer::DeleteKvRecord(std::string collection, std::string key, std::string trace_id) {
    if (!started_) {
        return kvai::infra::Status::Unavailable("server not started");
    }
    if (config_.read_only_mode) {
        return kvai::infra::Status::Unavailable("storage is in read-only mode");
    }
    collection = collection.empty() ? config_.default_collection : std::move(collection);
    if (key.empty()) {
        return kvai::infra::Status::InvalidArgument("kv key cannot be empty");
    }

    const auto trace = TraceInjector::EnsureTraceId(trace_id);
    const auto route = DescribeRoute(collection, key);
    if (route.has_primary && !route.local_owner) {
        return kvai::infra::Status::Unavailable("kv record owned by remote node: " + route.primary.id + " trace_id=" + trace);
    }

    return kv_store_->Delete(collection, key);
}

kvai::infra::Status InProcessGatewayServer::ReindexDocuments(std::string collection) {
    if (!started_) {
        return kvai::infra::Status::Unavailable("server not started");
    }
    collection = collection.empty() ? config_.default_collection : std::move(collection);

    constexpr std::size_t kPageSize = 1000;
    std::string begin_key;
    std::size_t indexed_count = 0;
    while (true) {
        auto records = kv_store_->Range(collection, begin_key, "", kPageSize);
        if (!records.ok()) {
            return records.status();
        }
        if (records.value().empty()) {
            break;
        }

        for (const auto& record : records.value()) {
            const auto image_reference = ImageReferenceForRecord(record);
            auto embedding = image_reference.empty()
                                 ? embedding_service_->EmbedText(record.title + " " + record.body)
                                 : embedding_service_->EmbedImage(image_reference);
            if (!embedding.ok()) {
                return embedding.status();
            }
            auto status = vector_index_->Upsert(record, embedding.value().values);
            if (!status.ok()) {
                return status;
            }
            ++indexed_count;
        }

        if (records.value().size() < kPageSize) {
            break;
        }
        begin_key = records.value().back().key;
        begin_key.push_back('\0');
    }

    auto status = vector_index_->FlushSnapshot();
    if (status.ok()) {
        kvai::infra::log::Info("gateway", "reindexed collection",
                               {{"collection", collection}, {"document_count", std::to_string(indexed_count)}});
    }
    return status;
}

kvai::infra::RouteDecision InProcessGatewayServer::DescribeRoute(const std::string& collection, const std::string& key) const {
    return router_.Route(collection.empty() ? config_.default_collection : collection, key, config_.replication_factor);
}

HealthReport InProcessGatewayServer::HealthCheck(std::string trace_id) const {
    auto report = service_->HealthCheck(TraceInjector::EnsureTraceId(trace_id));
    report.details.emplace("node_id", config_.node_id);
    report.details.emplace("discovery_backend", config_.discovery_backend);
    report.details.emplace("cluster_node_count", std::to_string(router_.NodeCount()));
    report.details.emplace("read_only_mode", config_.read_only_mode ? "true" : "false");
    report.details.emplace("tls_mode", config_.tls_mode);
    report.details.emplace("cpu_utilization_ratio", std::to_string(kvai::infra::MetricsRegistry::CpuUtilizationRatio()));
    report.details.emplace("disk_utilization_ratio", std::to_string(kvai::infra::MetricsRegistry::DiskUtilizationRatio(config_.wal_path)));
    report.details.emplace("thread_pool_workers", std::to_string(thread_pool_.WorkerCount()));
    report.details.emplace("thread_pool_pending_tasks", std::to_string(thread_pool_.PendingTasks()));
    return report;
}

kvai::infra::MetricsRegistry& InProcessGatewayServer::MutableMetrics() {
    return metrics_;
}

const kvai::infra::MetricsRegistry& InProcessGatewayServer::Metrics() const {
    return metrics_;
}

std::string InProcessGatewayServer::OpenApiSpec() const {
    std::ifstream input(config_.openapi_path);
    if (!input.is_open()) {
        return DefaultOpenApiSpec();
    }

    std::string content;
    std::string line;
    while (std::getline(input, line)) {
        content += line;
        content.push_back('\n');
    }
    return content.empty() ? DefaultOpenApiSpec() : content;
}

kvai::infra::Status InProcessGatewayServer::SeedDemoData() {
    if (!config_.enable_demo_data || config_.read_only_mode) {
        return kvai::infra::Status::Ok();
    }

    const auto existing = kv_store_->Range(config_.default_collection, "", "", 1);
    if (existing.ok() && !existing.value().empty()) {
        return kvai::infra::Status::Ok();
    }

    const std::vector<kvai::core::DocumentRecord> records = {
        {config_.default_collection, "doc-001", "Distributed KV Engine", "RocksDB inspired durable key value pipeline with write ahead log recovery and range scan support.", {{"domain", "storage"}, {"lang", "cpp"}}},
        {config_.default_collection, "doc-002", "Semantic Retrieval Worker", "Vector search worker converts text into dense embeddings and runs top k retrieval with graceful degradation.", {{"domain", "search"}, {"lang", "cpp"}}},
        {config_.default_collection, "doc-003", "Gateway Observability", "Gateway injects trace identifiers, enforces rate limits and exposes health status for production monitoring.", {{"domain", "gateway"}, {"lang", "cpp"}}},
    };

    auto status = kv_store_->BatchPut(records);
    if (!status.ok()) {
        return status;
    }

    for (const auto& record : records) {
        auto embedding = embedding_service_->EmbedText(record.title + " " + record.body);
        if (!embedding.ok()) {
            return embedding.status();
        }
        status = vector_index_->Upsert(record, embedding.value().values);
        if (!status.ok()) {
            return status;
        }
    }

    kv_store_->FlushSnapshot();
    vector_index_->FlushSnapshot();
    return kvai::infra::Status::Ok();
}

}  // namespace kvai::gateway
