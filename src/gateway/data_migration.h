#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ai/embedding_service.h"
#include "core/document.h"
#include "core/kv_store.h"
#include "infra/cluster_routing.h"
#include "infra/config.h"
#include "infra/status.h"
#include "search/vector_index.h"

namespace kvai::gateway {

struct DataMigrationStatus {
    bool enabled = false;
    std::string state = "disabled";
    std::size_t pending = 0;
    std::size_t inflight = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::size_t delayed_delete = 0;
    std::int64_t last_scan_unix_ms = 0;
    std::string last_error;
};

class DataMigrationManager {
public:
    DataMigrationManager(kvai::infra::ServerConfig config,
                         kvai::core::KvStore& kv_store,
                         kvai::ai::EmbeddingService& embedding_service,
                         kvai::search::VectorIndex& vector_index,
                         const kvai::infra::ConsistentHashRouter& router);
    ~DataMigrationManager();

    void Start();
    void Stop();
    void TriggerScan();
    [[nodiscard]] DataMigrationStatus Status() const;

private:
    struct Task {
        kvai::core::DocumentRecord record;
        kvai::infra::ClusterNode target;
        bool semantic = false;
        std::size_t attempts = 0;
        std::int64_t migration_epoch = 0;
        std::int64_t delete_after_unix_ms = 0;
        std::chrono::steady_clock::time_point delete_after;
    };

    void WorkerLoop();
    void ScanOnce();
    void ProcessDueDeletes();
    kvai::infra::Status MigrateTask(Task& task);
    kvai::infra::Status DeleteLocalIfStillRemote(const Task& task);
    kvai::infra::Status RecoverTasksLocked();
    kvai::infra::Status PersistTasksLocked() const;
    [[nodiscard]] bool IsSemanticRecord(const kvai::core::DocumentRecord& record) const;
    [[nodiscard]] std::string TaskKey(const kvai::core::DocumentRecord& record) const;
    [[nodiscard]] kvai::infra::Status PostMigrationRecord(const Task& task) const;
    void SetState(std::string state);
    void RecordError(const kvai::infra::Status& status);

    kvai::infra::ServerConfig config_;
    kvai::core::KvStore& kv_store_;
    kvai::ai::EmbeddingService& embedding_service_;
    kvai::search::VectorIndex& vector_index_;
    const kvai::infra::ConsistentHashRouter& router_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool running_ = false;
    bool stopping_ = false;
    bool scan_requested_ = false;
    std::thread worker_;

    std::map<std::string, Task> pending_;
    std::vector<Task> delayed_delete_;
    DataMigrationStatus status_;
};

kvai::infra::Status ApplyMigratedRecord(kvai::core::KvStore& kv_store,
                                        kvai::ai::EmbeddingService& embedding_service,
                                        kvai::search::VectorIndex& vector_index,
                                        const kvai::core::DocumentRecord& record,
                                        bool semantic);

}  // namespace kvai::gateway
