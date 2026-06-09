#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/service_types.h"
#include "infra/cluster_routing.h"
#include "infra/config.h"
#include "infra/status.h"
#include "core/document.h"

namespace kvai::gateway::json {

// --- Serialization ---

nlohmann::json ToJson(const HealthReport& report);
nlohmann::json ToJson(const SemanticSearchResult& result);
nlohmann::json ToJson(const kvai::infra::RouteDecision& route);
nlohmann::json ToJson(const kvai::core::DocumentRecord& record);
nlohmann::json ToJson(const std::vector<kvai::core::DocumentRecord>& records);

// --- Deserialization ---

kvai::infra::StatusOr<SemanticSearchQuery> ParseSearchQuery(const nlohmann::json& j, bool is_post);
kvai::infra::StatusOr<kvai::core::DocumentRecord> ParseDocumentUpsert(const nlohmann::json& j);

// --- HTTP Response Helpers ---

std::string BuildHttpResponse(int status_code, const std::string& status_text,
                              const std::string& content_type, const std::string& body);

}  // namespace kvai::gateway::json
