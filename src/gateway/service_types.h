#pragma once

#include <map>
#include <string>
#include <vector>

namespace kvai::gateway {

struct SearchFilter {
    std::string field;
    std::string value;
};

struct SemanticSearchQuery {
    std::string trace_id;
    std::string collection;
    std::string query;
    std::size_t top_k = 10;
    std::vector<SearchFilter> filters;
};

struct SearchHit {
    std::string key;
    std::string title;
    std::string snippet;
    double score = 0.0;
    std::map<std::string, std::string> metadata;
};

struct SemanticSearchResult {
    std::string trace_id;
    std::vector<SearchHit> hits;
    bool degraded = false;
    std::string message;
};

struct HealthReport {
    std::string trace_id;
    std::string status;
    std::string version;
    std::vector<std::string> warnings;
    std::map<std::string, std::string> details;
};

}  // namespace kvai::gateway