#include "gateway/json_helpers.h"

#include <sstream>

namespace kvai::gateway::json {

// --- Serialization ---

nlohmann::json ToJson(const HealthReport& report) {
    nlohmann::json j;
    j["trace_id"] = report.trace_id;
    j["status"] = report.status;
    j["version"] = report.version;
    j["warnings"] = report.warnings;
    j["details"] = report.details;
    return j;
}

nlohmann::json ToJson(const SemanticSearchResult& result) {
    nlohmann::json j;
    j["trace_id"] = result.trace_id;
    j["degraded"] = result.degraded;
    j["message"] = result.message;

    auto hits = nlohmann::json::array();
    for (const auto& hit : result.hits) {
        nlohmann::json h;
        h["key"] = hit.key;
        h["title"] = hit.title;
        h["snippet"] = hit.snippet;
        h["score"] = hit.score;
        h["metadata"] = hit.metadata;
        hits.push_back(h);
    }
    j["hits"] = hits;
    return j;
}

nlohmann::json ToJson(const kvai::infra::RouteDecision& route) {
    nlohmann::json j;
    j["has_primary"] = route.has_primary;
    j["local_owner"] = route.local_owner;
    j["slot_id"] = route.slot_id;

    nlohmann::json primary;
    primary["id"] = route.primary.id;
    primary["host"] = route.primary.host;
    primary["port"] = route.primary.port;
    primary["weight"] = route.primary.weight;
    primary["zone"] = route.primary.zone;
    j["primary"] = primary;

    auto replicas = nlohmann::json::array();
    for (const auto& replica : route.replicas) {
        nlohmann::json r;
        r["id"] = replica.id;
        r["host"] = replica.host;
        r["port"] = replica.port;
        r["weight"] = replica.weight;
        r["zone"] = replica.zone;
        replicas.push_back(r);
    }
    j["replicas"] = replicas;
    return j;
}

nlohmann::json ToJson(const kvai::core::DocumentRecord& record) {
    nlohmann::json j;
    j["collection"] = record.collection;
    j["key"] = record.key;
    j["title"] = record.title;
    j["body"] = record.body;
    j["value"] = record.body;
    j["metadata"] = record.metadata;
    j["version"] = record.version;
    j["updated_at_unix_ms"] = record.updated_at_unix_ms;
    j["mutation_id"] = record.mutation_id;
    return j;
}

nlohmann::json ToJson(const std::vector<kvai::core::DocumentRecord>& records) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& record : records) {
        items.push_back(ToJson(record));
    }

    nlohmann::json j;
    j["items"] = std::move(items);
    return j;
}

// --- Deserialization ---

kvai::infra::StatusOr<SemanticSearchQuery> ParseSearchQuery(const nlohmann::json& j, bool is_post) {
    SemanticSearchQuery query;

    if (is_post) {
        if (j.contains("query") && j["query"].is_string()) {
            query.query = j["query"].get<std::string>();
        }
        if (j.contains("image_path") && j["image_path"].is_string()) {
            query.image_reference = j["image_path"].get<std::string>();
        } else if (j.contains("image_reference") && j["image_reference"].is_string()) {
            query.image_reference = j["image_reference"].get<std::string>();
        }
        if (j.contains("collection") && j["collection"].is_string()) {
            query.collection = j["collection"].get<std::string>();
        }
        if (j.contains("top_k") && j["top_k"].is_number()) {
            query.top_k = j["top_k"].get<std::size_t>();
        }
        if (j.contains("filters") && j["filters"].is_object()) {
            for (auto& [key, value] : j["filters"].items()) {
                if (value.is_string()) {
                    query.filters.push_back(SearchFilter{key, value.get<std::string>()});
                }
            }
        }
    } else {
        // GET: query params are already parsed into the query object externally
        // This overload is for POST body parsing only
    }

    return query;
}

kvai::infra::StatusOr<kvai::core::DocumentRecord> ParseDocumentUpsert(const nlohmann::json& j) {
    kvai::core::DocumentRecord record;

    if (j.contains("collection") && j["collection"].is_string()) {
        record.collection = j["collection"].get<std::string>();
    }
    if (j.contains("key") && j["key"].is_string()) {
        record.key = j["key"].get<std::string>();
    }
    if (j.contains("title") && j["title"].is_string()) {
        record.title = j["title"].get<std::string>();
    }
    if (j.contains("body") && j["body"].is_string()) {
        record.body = j["body"].get<std::string>();
    } else if (j.contains("value") && j["value"].is_string()) {
        record.body = j["value"].get<std::string>();
    }
    if (j.contains("metadata") && j["metadata"].is_object()) {
        for (auto& [key, value] : j["metadata"].items()) {
            if (value.is_string()) {
                record.metadata[key] = value.get<std::string>();
            }
        }
    }
    if (j.contains("version") && j["version"].is_number_unsigned()) {
        record.version = j["version"].get<std::uint64_t>();
    }
    if (j.contains("updated_at_unix_ms") && j["updated_at_unix_ms"].is_number_integer()) {
        record.updated_at_unix_ms = j["updated_at_unix_ms"].get<std::int64_t>();
    }
    if (j.contains("mutation_id") && j["mutation_id"].is_string()) {
        record.mutation_id = j["mutation_id"].get<std::string>();
    }

    return record;
}

// --- HTTP Response Helpers ---

std::string BuildHttpResponse(int status_code, const std::string& status_text,
                              const std::string& content_type, const std::string& body) {
    std::ostringstream output;
    output << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n"
           << body;
    return output.str();
}

}  // namespace kvai::gateway::json
