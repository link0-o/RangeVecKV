#include "gateway/data_migration.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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

kvai::infra::Status EnsureParentDirectory(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        return kvai::infra::Status::Ok();
    }
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        return kvai::infra::Status::Internal("failed to create migration task wal directory: " + error.message());
    }
    return kvai::infra::Status::Ok();
}

std::chrono::steady_clock::time_point SteadyFromUnixMillis(std::int64_t unix_ms) {
    const auto now_unix = UnixMillis();
    const auto now_steady = std::chrono::steady_clock::now();
    if (unix_ms <= now_unix) {
        return now_steady;
    }
    return now_steady + std::chrono::milliseconds(unix_ms - now_unix);
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
    auto recover_status = RecoverTasksLocked();
    if (!recover_status.ok()) {
        status_.state = "degraded";
        status_.last_error = recover_status.ToString();
    }
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
                task.migration_epoch = UnixMillis();
                task.delete_after_unix_ms = task.migration_epoch +
                                            static_cast<std::int64_t>(config_.migration_delete_delay_ms);
                task.delete_after = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(config_.migration_delete_delay_ms);

                const auto key = TaskKey(record);
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_.find(key) == pending_.end()) {
                    pending_.emplace(key, std::move(task));
                    auto persist_status = PersistTasksLocked();
                    if (!persist_status.ok()) {
                        status_.state = "degraded";
                        status_.last_error = persist_status.ToString();
                    }
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
                task.delete_after_unix_ms = UnixMillis() + static_cast<std::int64_t>(config_.migration_delete_delay_ms);
                task.delete_after = SteadyFromUnixMillis(task.delete_after_unix_ms);
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
            auto persist_status = PersistTasksLocked();
            if (!persist_status.ok()) {
                status_.state = "degraded";
                status_.last_error = persist_status.ToString();
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
    if (!due.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto persist_status = PersistTasksLocked();
        if (!persist_status.ok()) {
            status_.state = "degraded";
            status_.last_error = persist_status.ToString();
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

kvai::infra::Status DataMigrationManager::RecoverTasksLocked() {
    std::ifstream input(config_.migration_task_wal_path);
    if (!input.is_open()) {
        return kvai::infra::Status::Ok();
    }

    nlohmann::json root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded()) {
        return kvai::infra::Status::Internal("failed to parse migration task wal");
    }
    if (!root.is_array()) {
        return kvai::infra::Status::InvalidArgument("migration task wal must contain a JSON array");
    }

    pending_.clear();
    delayed_delete_.clear();
    for (const auto& item : root) {
        if (!item.is_object()) {
            continue;
        }

        Task task;
        task.record.collection = item.value("collection", "");
        task.record.key = item.value("key", "");
        task.record.title = item.value("title", "");
        task.record.body = item.value("body", "");
        if (item.contains("metadata") && item["metadata"].is_object()) {
            for (auto& [key, value] : item["metadata"].items()) {
                if (value.is_string()) {
                    task.record.metadata[key] = value.get<std::string>();
                }
            }
        }
        task.record.version = item.value("version", static_cast<std::uint64_t>(0));
        task.record.updated_at_unix_ms = item.value("updated_at_unix_ms", static_cast<std::int64_t>(0));
        task.record.mutation_id = item.value("mutation_id", "");
        task.target.id = item.value("target_id", "");
        task.target.host = item.value("target_host", "");
        task.target.port = static_cast<std::uint16_t>(item.value("target_port", 0));
        task.target.healthy = item.value("target_healthy", true);
        task.target.weight = item.value("target_weight", 100U);
        task.target.zone = item.value("target_zone", "");
        task.semantic = item.value("semantic", false);
        task.attempts = item.value("attempts", static_cast<std::size_t>(0));
        task.migration_epoch = item.value("migration_epoch", static_cast<std::int64_t>(0));
        task.delete_after_unix_ms = item.value("delete_after_unix_ms", static_cast<std::int64_t>(0));
        task.delete_after = SteadyFromUnixMillis(task.delete_after_unix_ms);

        if (task.record.collection.empty() || task.record.key.empty()) {
            continue;
        }
        const auto state = item.value("state", "pending");
        if (state == "delete_pending") {
            delayed_delete_.push_back(std::move(task));
        } else {
            pending_[TaskKey(task.record)] = std::move(task);
        }
    }

    status_.pending = pending_.size();
    status_.delayed_delete = delayed_delete_.size();
    return kvai::infra::Status::Ok();
}

kvai::infra::Status DataMigrationManager::PersistTasksLocked() const {
    auto status = EnsureParentDirectory(config_.migration_task_wal_path);
    if (!status.ok()) {
        return status;
    }

    nlohmann::json root = nlohmann::json::array();
    auto append_task = [&root](const Task& task, const char* state) {
        nlohmann::json item;
        item["state"] = state;
        item["collection"] = task.record.collection;
        item["key"] = task.record.key;
        item["title"] = task.record.title;
        item["body"] = task.record.body;
        item["metadata"] = task.record.metadata;
        item["version"] = task.record.version;
        item["updated_at_unix_ms"] = task.record.updated_at_unix_ms;
        item["mutation_id"] = task.record.mutation_id;
        item["target_id"] = task.target.id;
        item["target_host"] = task.target.host;
        item["target_port"] = task.target.port;
        item["target_healthy"] = task.target.healthy;
        item["target_weight"] = task.target.weight;
        item["target_zone"] = task.target.zone;
        item["semantic"] = task.semantic;
        item["attempts"] = task.attempts;
        item["migration_epoch"] = task.migration_epoch;
        item["delete_after_unix_ms"] = task.delete_after_unix_ms;
        root.push_back(std::move(item));
    };

    for (const auto& [_, task] : pending_) {
        (void)_;
        append_task(task, "pending");
    }
    for (const auto& task : delayed_delete_) {
        append_task(task, "delete_pending");
    }

    std::ofstream output(config_.migration_task_wal_path, std::ios::trunc);
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open migration task wal");
    }
    output << root.dump(2);
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
    body["version"] = task.record.version;
    body["updated_at_unix_ms"] = task.record.updated_at_unix_ms;
    body["mutation_id"] = task.record.mutation_id;
    body["semantic"] = task.semantic;
    body["migration_epoch"] = task.migration_epoch == 0 ? UnixMillis() : task.migration_epoch;

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
