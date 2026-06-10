#pragma once

#include <map>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kvai::core {

struct DocumentRecord {
    DocumentRecord() = default;
    DocumentRecord(std::string collection_value,
                   std::string key_value,
                   std::string title_value,
                   std::string body_value,
                   std::map<std::string, std::string> metadata_value = {},
                   std::uint64_t version_value = 0,
                   std::int64_t updated_at_unix_ms_value = 0,
                   std::string mutation_id_value = {})
        : collection(std::move(collection_value)),
          key(std::move(key_value)),
          title(std::move(title_value)),
          body(std::move(body_value)),
          metadata(std::move(metadata_value)),
          version(version_value),
          updated_at_unix_ms(updated_at_unix_ms_value),
          mutation_id(std::move(mutation_id_value)) {}

    std::string collection;
    std::string key;
    std::string title;
    std::string body;
    std::map<std::string, std::string> metadata;
    std::uint64_t version = 0;
    std::int64_t updated_at_unix_ms = 0;
    std::string mutation_id;
};

struct SearchReference {
    std::string collection;
    std::string key;
    double score = 0.0;
    std::map<std::string, std::string> metadata;
};

std::string BuildCompositeKey(const std::string& collection, const std::string& key);

}  // namespace kvai::core
