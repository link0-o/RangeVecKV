#include "gateway/data_migration.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#include <nlohmann/json.hpp>

#include "infra/logging.h"

namespace kvai::gateway {

namespace {

std::int64_t UnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string HttpStatusLine(const std::string& response) {
    const auto end = response.find("\r\n");
    return end == std::string::npos ? response : response.substr(0, end);
}

kvai::infra::Status SendHttpPost(const std::string& host,
                                 std::uint16_t port,
                                 const std::string& path,
                                 const std::string& body,
                                 const std::vector<std::string>& headers) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const auto port_text = std::to_string(port);
    const int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
    if (gai != 0) {
        return kvai::infra::Status::Unavailable("migration target resolve failed: " + std::string(::gai_strerror(gai)));
    }

    int fd = -1;
    for (auto* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
        fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);

    if (fd < 0) {
        return kvai::infra::Status::Unavailable("migration target connect failed: " + host + ":" + port_text);
    }

    std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& header : headers) {
        request += header + "\r\n";
    }
    request += "\r\n";
    request += body;

    const auto sent = ::send(fd, request.data(), request.size(), 0);
    if (sent < 0 || static_cast<std::size_t>(sent) != request.size()) {
        ::close(fd);
        return kvai::infra::Status::Unavailable("migration target send failed");
    }

    std::string response;
    char buffer[2048];
    while (true) {
        const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(fd);

    const auto status_line = HttpStatusLine(response);
    if (status_line.find(" 200 ") == std::string::npos) {
        return kvai::infra::Status::Unavailable("migration target rejected record: " + status_line);
    }
    return kvai::infra::Status::Ok();
}

}  // namespace

DataMigrationManager::DataMigrationManager(kvai::infra::ServerConfig config,
                                           kvai::core::KvStore& kv_store,
                                           kvai::ai::EmbeddingService& embedding_service,
                                           kvai::search::VectorIndex& vector_index,
                                           const kvai::infra::ConsistentHashRouter& router)
    : config_(std::move(config)),
      kv_store_(kv_store),
      embedding_service_(embedding_service),
      vector_index_(vector_index),
      router_(router) {
    status_.enabled = config_.data_migration_enabled;
    status_.state = config_.data_migration_enabled ? "idle" : "disabled";
}

DataMigrationManager::~DataMigrationManager() {
    Stop();
}

void DataMigrationManager::Start() {
    if (!config_.data_migration_enabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    stopping_ = false;
    running_ = true;
    status_.enabled = true;
    status_.state = "idle";
    worker_ = std::thread([this]() { WorkerLoop(); });
}

void DataMigrationManager::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stopping_ = true;
        scan_requested_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    status_.state = "stopped";
}

void DataMigrationManager::TriggerScan() {
    if (!config_.data_migration_enabled) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scan_requested_ = true;
    }
    condition_.notify_all();
}

DataMigrationStatus DataMigrationManager::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = status_;
    status.pending = pending_.size();
    status.delayed_delete = delayed_delete_.size();
    return status;
}

void DataMigrationManager::WorkerLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, std::chrono::seconds(1), [this]() { return stopping_ || scan_requested_; });
            if (stopping_) {
                return;
            }
            scan_requested_ = false;
        }

        ScanOnce();
        ProcessDueDeletes();
    }
}

void DataMigrationManager::ScanOnce() {
    SetState("scanning");
    auto collections = kv_store_.Collections();
    if (!collections.ok()) {
        RecordError(collections.status());
        return;
    }

    for (const auto& collection : collections.value()) {
        std::string begin_key;
        while (true) {
            auto records = kv_store_.Range(collection, begin_key, "", config_.migration_batch_size);
            if (!records.ok()) {
                RecordError(records.status());
                return;
            }
            if (records.value().empty()) {
                break;
            }

            for (const auto& record : records.value()) {
                auto route = router_.Route(record.collection, record.key, config_.replication_factor);
                if (!route.has_primary || route.local_owner) {
                    continue;
                }

                Task task;
                task.record = record;
                task.target = route.primary;
                task.semantic = IsSemanticRecord(record);
                task.delete_after = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(config_.migration_delete_delay_ms);

                const auto key = TaskKey(record);
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_.find(key) == pending_.end()) {
                    pending_.emplace(key, std::move(task));
                }
            }

            if (records.value().size() < config_.migration_batch_size) {
                break;
            }
            begin_key = records.value().back().key;
            begin_key.push_back('\0');
        }
    }

    std::vector<std::string> completed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.last_scan_unix_ms = UnixMillis();
        status_.state = "migrating";
        status_.inflight = 0;
    }

    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        keys.reserve(pending_.size());
        for (const auto& [key, _] : pending_) {
            (void)_;
            keys.push_back(key);
        }
    }

    for (const auto& key : keys) {
        Task task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto iterator = pending_.find(key);
            if (iterator == pending_.end()) {
                continue;
            }
            ++status_.inflight;
            task = iterator->second;
        }

        auto status = MigrateTask(task);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status_.inflight > 0) {
                --status_.inflight;
            }
            auto iterator = pending_.find(key);
            if (iterator == pending_.end()) {
                continue;
            }
            if (status.ok()) {
                ++status_.succeeded;
                delayed_delete_.push_back(task);
                pending_.erase(iterator);
            } else {
                ++iterator->second.attempts;
                if (iterator->second.attempts >= config_.migration_max_retries) {
                    ++status_.failed;
                    status_.last_error = status.ToString();
                    pending_.erase(iterator);
                } else {
                    status_.last_error = status.ToString();
                }
            }
        }
    }

    SetState("idle");
}

void DataMigrationManager::ProcessDueDeletes() {
    std::vector<Task> due;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto iterator = delayed_delete_.begin();
        while (iterator != delayed_delete_.end()) {
            if (iterator->delete_after <= now) {
                due.push_back(*iterator);
                iterator = delayed_delete_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    for (const auto& task : due) {
        auto status = DeleteLocalIfStillRemote(task);
        if (!status.ok()) {
            RecordError(status);
        }
    }
}

kvai::infra::Status DataMigrationManager::MigrateTask(Task& task) {
    if (task.target.host.empty() || task.target.port == 0) {
        return kvai::infra::Status::Unavailable("migration target endpoint is empty");
    }
    return PostMigrationRecord(task);
}

kvai::infra::Status DataMigrationManager::DeleteLocalIfStillRemote(const Task& task) {
    auto route = router_.Route(task.record.collection, task.record.key, config_.replication_factor);
    if (!route.has_primary || route.local_owner) {
        return kvai::infra::Status::Ok();
    }
    auto status = kv_store_.Delete(task.record.collection, task.record.key);
    if (!status.ok()) {
        return status;
    }
    if (task.semantic) {
        return vector_index_.Remove(task.record.collection, task.record.key);
    }
    return kvai::infra::Status::Ok();
}

bool DataMigrationManager::IsSemanticRecord(const kvai::core::DocumentRecord& record) const {
    if (!record.title.empty()) {
        return true;
    }
    if (record.collection == config_.default_collection) {
        return true;
    }
    for (const auto* key : {"image_path", "image_reference", "image_uri"}) {
        if (record.metadata.find(key) != record.metadata.end()) {
            return true;
        }
    }
    return false;
}

std::string DataMigrationManager::TaskKey(const kvai::core::DocumentRecord& record) const {
    return record.collection + '\0' + record.key;
}

kvai::infra::Status DataMigrationManager::PostMigrationRecord(const Task& task) const {
    nlohmann::json body;
    body["collection"] = task.record.collection;
    body["key"] = task.record.key;
    body["title"] = task.record.title;
    body["body"] = task.record.body;
    body["metadata"] = task.record.metadata;
    body["semantic"] = task.semantic;
    body["migration_epoch"] = UnixMillis();

    std::vector<std::string> headers = {"x-kvai-internal-migration: 1"};
    if (!config_.api_key.empty()) {
        headers.push_back("x-api-key: " + config_.api_key);
    }
    return SendHttpPost(task.target.host, task.target.port, "/internal/migration/records", body.dump(), headers);
}

void DataMigrationManager::SetState(std::string state) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = std::move(state);
}

void DataMigrationManager::RecordError(const kvai::infra::Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = "degraded";
    status_.last_error = status.ToString();
}

kvai::infra::Status ApplyMigratedRecord(kvai::core::KvStore& kv_store,
                                        kvai::ai::EmbeddingService& embedding_service,
                                        kvai::search::VectorIndex& vector_index,
                                        const kvai::core::DocumentRecord& record,
                                        bool semantic) {
    if (record.collection.empty() || record.key.empty()) {
        return kvai::infra::Status::InvalidArgument("migrated record requires collection and key");
    }

    auto status = kv_store.Put(record);
    if (!status.ok()) {
        return status;
    }

    if (!semantic) {
        return kvai::infra::Status::Ok();
    }

    auto embedding = embedding_service.EmbedText(record.title + " " + record.body);
    if (!embedding.ok()) {
        return embedding.status();
    }
    return vector_index.Upsert(record, embedding.value().values);
}

}  // namespace kvai::gateway
