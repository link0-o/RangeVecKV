#include "gateway/vector_index_outbox.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>

#include "core/persistence_codec.h"
#include "infra/logging.h"

namespace kvai::gateway {

namespace {

kvai::infra::Status EnsureParentDirectory(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        return kvai::infra::Status::Ok();
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        return kvai::infra::Status::Internal("failed to create vector outbox directory: " + error.message());
    }
    return kvai::infra::Status::Ok();
}

kvai::infra::StatusOr<std::string> ReadWholeFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return kvai::infra::Status::NotFound("vector outbox file not found");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
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

VectorIndexOutbox::VectorIndexOutbox(kvai::infra::ServerConfig config,
                                     kvai::ai::EmbeddingService& embedding_service,
                                     kvai::search::VectorIndex& vector_index)
    : config_(std::move(config)), embedding_service_(embedding_service), vector_index_(vector_index) {}

VectorIndexOutbox::~VectorIndexOutbox() {
    Stop();
}

kvai::infra::Status VectorIndexOutbox::Recover() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();

    auto bytes = ReadWholeFile(config_.vector_index_outbox_path);
    if (!bytes.ok()) {
        return kvai::infra::Status::Ok();
    }
    if (bytes.value().empty()) {
        return kvai::infra::Status::Ok();
    }

    auto frames = kvai::core::persistence::DecodeFrames(bytes.value());
    if (!frames.ok()) {
        return frames.status();
    }
    for (const auto& frame : frames.value()) {
        auto record = kvai::core::persistence::DecodeDocumentRecord(frame);
        if (!record.ok()) {
            return record.status();
        }
        pending_[TaskKey(record.value())] = record.value();
    }
    status_.pending = pending_.size();
    return kvai::infra::Status::Ok();
}

void VectorIndexOutbox::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    stopping_ = false;
    worker_ = std::thread([this]() { WorkerLoop(); });
}

void VectorIndexOutbox::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    status_.state = "stopped";
}

kvai::infra::Status VectorIndexOutbox::Enqueue(const kvai::core::DocumentRecord& record) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[TaskKey(record)] = record;
        status_.pending = pending_.size();
        status_.state = "pending";
        auto status = RewriteLocked();
        if (!status.ok()) {
            RecordError(status);
            return status;
        }
    }
    condition_.notify_all();
    return kvai::infra::Status::Ok();
}

kvai::infra::Status VectorIndexOutbox::Remove(const std::string& collection, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(kvai::core::BuildCompositeKey(collection, key));
    status_.pending = pending_.size();
    return RewriteLocked();
}

kvai::infra::Status VectorIndexOutbox::DrainOnce() {
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = pending_.empty() ? "idle" : "indexing";
        keys.reserve(pending_.size());
        for (const auto& [key, _] : pending_) {
            (void)_;
            keys.push_back(key);
        }
    }

    for (const auto& key : keys) {
        kvai::core::DocumentRecord record;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto iterator = pending_.find(key);
            if (iterator == pending_.end()) {
                continue;
            }
            record = iterator->second;
        }

        auto status = IndexRecord(record);
        std::lock_guard<std::mutex> lock(mutex_);
        if (status.ok()) {
            pending_.erase(key);
            ++status_.succeeded;
            status_.pending = pending_.size();
            status_.state = pending_.empty() ? "idle" : "pending";
            auto rewrite = RewriteLocked();
            if (!rewrite.ok()) {
                status_.state = "degraded";
                status_.last_error = rewrite.ToString();
                return rewrite;
            }
        } else {
            ++status_.failed;
            status_.state = "degraded";
            status_.last_error = status.ToString();
        }
    }

    return kvai::infra::Status::Ok();
}

VectorIndexOutboxStatus VectorIndexOutbox::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = status_;
    status.pending = pending_.size();
    return status;
}

std::string VectorIndexOutbox::TaskKey(const kvai::core::DocumentRecord& record) const {
    return kvai::core::BuildCompositeKey(record.collection, record.key);
}

kvai::infra::Status VectorIndexOutbox::IndexRecord(const kvai::core::DocumentRecord& record) {
    const auto image_reference = ImageReferenceForRecord(record);
    auto embedding = image_reference.empty()
                         ? embedding_service_.EmbedText(record.title + " " + record.body)
                         : embedding_service_.EmbedImage(image_reference);
    if (!embedding.ok()) {
        return embedding.status();
    }
    return vector_index_.Upsert(record, embedding.value().values);
}

kvai::infra::Status VectorIndexOutbox::RewriteLocked() const {
    auto status = EnsureParentDirectory(config_.vector_index_outbox_path);
    if (!status.ok()) {
        return status;
    }

    std::ofstream output(config_.vector_index_outbox_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open vector index outbox");
    }
    output << kvai::core::persistence::MagicHeader();
    for (const auto& [_, record] : pending_) {
        (void)_;
        output << kvai::core::persistence::EncodeFrame(kvai::core::persistence::EncodeDocumentRecord(record));
    }
    return kvai::infra::Status::Ok();
}

void VectorIndexOutbox::WorkerLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, std::chrono::seconds(1), [this]() { return stopping_ || !pending_.empty(); });
            if (stopping_) {
                return;
            }
        }
        auto status = DrainOnce();
        if (!status.ok()) {
            kvai::infra::log::Warn("gateway", "vector index outbox drain failed", {{"error", status.ToString()}});
        }
    }
}

void VectorIndexOutbox::RecordError(const kvai::infra::Status& status) {
    status_.state = "degraded";
    status_.last_error = status.ToString();
}

}  // namespace kvai::gateway
