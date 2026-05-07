#pragma once

#include <map>
#include <string>
#include <vector>

namespace kvai::core {

struct DocumentRecord {
    std::string collection;
    std::string key;
    std::string title;
    std::string body;
    std::map<std::string, std::string> metadata;
};

struct SearchReference {
    std::string collection;
    std::string key;
    double score = 0.0;
    std::map<std::string, std::string> metadata;
};

std::string BuildCompositeKey(const std::string& collection, const std::string& key);

}  // namespace kvai::core