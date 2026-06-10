#pragma once

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "ai/embedding_service.h"
#include "core/document.h"
#include "infra/config.h"
#include "infra/status.h"
#include "search/vector_index.h"

namespace kvai::gateway {

struct VectorIndexOutboxStatus {
    bool enabled = true;
    std::string state = "idle";
    std::size_t pending = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::string last_error;
};

class VectorIndexOutbox {
public:
    VectorIndexOutbox(kvai::infra::ServerConfig config,
                      kvai::ai::EmbeddingService& embedding_service,
                      kvai::search::VectorIndex& vector_index);
    ~VectorIndexOutbox();

    kvai::infra::Status Recover();
    void Start();
    void Stop();
    kvai::infra::Status Enqueue(const kvai::core::DocumentRecord& record);
    kvai::infra::Status Remove(const std::string& collection, const std::string& key);
    kvai::infra::Status DrainOnce();
    [[nodiscard]] VectorIndexOutboxStatus Status() const;

private:
    [[nodiscard]] std::string TaskKey(const kvai::core::DocumentRecord& record) const;
    [[nodiscard]] kvai::infra::Status IndexRecord(const kvai::core::DocumentRecord& record);
    kvai::infra::Status RewriteLocked() const;
    void WorkerLoop();
    void RecordError(const kvai::infra::Status& status);

    kvai::infra::ServerConfig config_;
    kvai::ai::EmbeddingService& embedding_service_;
    kvai::search::VectorIndex& vector_index_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool running_ = false;
    bool stopping_ = false;
    std::thread worker_;
    std::map<std::string, kvai::core::DocumentRecord> pending_;
    VectorIndexOutboxStatus status_;
};

}  // namespace kvai::gateway
