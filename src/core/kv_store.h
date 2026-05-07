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

namespace kvai::core {

class KvStore {
public:
    virtual ~KvStore() = default;

    virtual kvai::infra::Status Recover() = 0;
    virtual kvai::infra::Status Put(const DocumentRecord& record) = 0;
    virtual kvai::infra::Status BatchPut(const std::vector<DocumentRecord>& records) = 0;
    virtual kvai::infra::Status Delete(const std::string& collection, const std::string& key) = 0;
    virtual kvai::infra::StatusOr<DocumentRecord> Get(const std::string& collection, const std::string& key) const = 0;
    virtual kvai::infra::StatusOr<std::vector<DocumentRecord>> MultiGet(const std::vector<SearchReference>& references) const = 0;
    virtual kvai::infra::StatusOr<std::vector<DocumentRecord>> Range(const std::string& collection,
                                                                     const std::string& begin_key,
                                                                     const std::string& end_key,
                                                                     std::size_t limit) const = 0;
    virtual kvai::infra::Status FlushSnapshot() = 0;
};

class WriteAheadKvStore final : public KvStore {
public:
    WriteAheadKvStore(std::string wal_path, std::string snapshot_path);
    ~WriteAheadKvStore() override;

    kvai::infra::Status Recover() override;
    kvai::infra::Status Put(const DocumentRecord& record) override;
    kvai::infra::Status BatchPut(const std::vector<DocumentRecord>& records) override;
    kvai::infra::Status Delete(const std::string& collection, const std::string& key) override;
    kvai::infra::StatusOr<DocumentRecord> Get(const std::string& collection, const std::string& key) const override;
    kvai::infra::StatusOr<std::vector<DocumentRecord>> MultiGet(const std::vector<SearchReference>& references) const override;
    kvai::infra::StatusOr<std::vector<DocumentRecord>> Range(const std::string& collection,
                                                             const std::string& begin_key,
                                                             const std::string& end_key,
                                                             std::size_t limit) const override;
    kvai::infra::Status FlushSnapshot() override;

private:
    kvai::infra::Status ReplaySnapshotLocked();
    kvai::infra::Status ReplayWalLocked();
    kvai::infra::Status AppendWalLocked(char operation, const DocumentRecord& record);
    kvai::infra::Status AppendDeleteLocked(const std::string& collection, const std::string& key);

    std::string wal_path_;
    std::string snapshot_path_;
    mutable std::mutex mutex_;
    std::map<std::string, DocumentRecord> records_;
};

kvai::infra::StatusOr<std::unique_ptr<KvStore>> CreateKvStore(const kvai::infra::ServerConfig& config);

}  // namespace kvai::core