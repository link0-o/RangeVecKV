#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/document.h"
#include "infra/config.h"
#include "infra/status.h"

namespace kvai::search {

struct SearchFilter {
    std::string field;
    std::string value;
};

class VectorIndex {
public:
    virtual ~VectorIndex() = default;

    virtual kvai::infra::Status Recover() = 0;
    virtual kvai::infra::Status Upsert(const kvai::core::DocumentRecord& record, const std::vector<float>& vector) = 0;
    virtual kvai::infra::Status Remove(const std::string& collection, const std::string& key) = 0;
    virtual kvai::infra::StatusOr<std::vector<kvai::core::SearchReference>> Search(const std::string& collection,
                                                                                    const std::vector<float>& query_vector,
                                                                                    const std::vector<SearchFilter>& filters,
                                                                                    std::size_t top_k) const = 0;
    virtual kvai::infra::Status FlushSnapshot() = 0;
};

class PersistentVectorIndex final : public VectorIndex {
public:
    explicit PersistentVectorIndex(std::string snapshot_path);
    ~PersistentVectorIndex() override;

    kvai::infra::Status Recover() override;
    kvai::infra::Status Upsert(const kvai::core::DocumentRecord& record, const std::vector<float>& vector) override;
    kvai::infra::Status Remove(const std::string& collection, const std::string& key) override;
    kvai::infra::StatusOr<std::vector<kvai::core::SearchReference>> Search(const std::string& collection,
                                                                            const std::vector<float>& query_vector,
                                                                            const std::vector<SearchFilter>& filters,
                                                                            std::size_t top_k) const override;
    kvai::infra::Status FlushSnapshot() override;

private:
    struct Entry {
        kvai::core::DocumentRecord record;
        std::vector<float> vector;
    };

    static bool MatchesFilters(const Entry& entry, const std::vector<SearchFilter>& filters);
    static double CosineSimilarity(const std::vector<float>& lhs, const std::vector<float>& rhs);

    std::string snapshot_path_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

kvai::infra::StatusOr<std::unique_ptr<VectorIndex>> CreateVectorIndex(const kvai::infra::ServerConfig& config);

}  // namespace kvai::search