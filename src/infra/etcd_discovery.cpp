#include "infra/etcd_discovery.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(KVAI_HAVE_ETCD)
#include <etcd/Client.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/KeepAlive.hpp>
#endif

#include "infra/logging.h"

namespace kvai::infra {

#if defined(KVAI_HAVE_ETCD)

struct EtcdServiceDiscovery::EtcdClientState {
    std::unique_ptr<etcd::Client> client;
    std::unique_ptr<etcd::KeepAlive> keep_alive;
    std::unique_ptr<etcd::Watcher> watcher;
    int64_t lease_id = 0;
};

#endif

namespace {

#if defined(KVAI_HAVE_ETCD)

std::int64_t UnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string PrefixRangeEnd(const std::string& prefix) {
    if (prefix.empty()) {
        return std::string(1, '\0');
    }

    std::string end = prefix;
    for (auto iterator = end.rbegin(); iterator != end.rend(); ++iterator) {
        auto byte = static_cast<unsigned char>(*iterator);
        if (byte != 0xFFU) {
            *iterator = static_cast<char>(byte + 1U);
            end.resize(static_cast<std::size_t>(std::distance(iterator, end.rend())));
            return end;
        }
    }
    return std::string(1, '\0');
}

#endif

StatusOr<ClusterNode> ParseLegacyNodeValue(const std::string& node_id, const std::string& value) {
    ClusterNode node;
    node.id = node_id;
    const auto colon = value.rfind(':');
    if (colon == std::string::npos) {
        return Status::InvalidArgument("legacy etcd node value must be host:port");
    }
    node.host = value.substr(0, colon);
    try {
        const auto parsed_port = std::stoul(value.substr(colon + 1));
        if (parsed_port == 0 || parsed_port > 65535) {
            return Status::InvalidArgument("legacy etcd node port out of range");
        }
        node.port = static_cast<std::uint16_t>(parsed_port);
    } catch (const std::exception& error) {
        return Status::InvalidArgument(std::string("invalid legacy etcd node port: ") + error.what());
    }
    if (node.host.empty()) {
        return Status::InvalidArgument("legacy etcd node host cannot be empty");
    }
    return node;
}

}  // namespace

std::string SerializeEtcdNodeValue(const ClusterNode& node) {
    nlohmann::json value;
    value["version"] = 1;
    value["id"] = node.id;
    value["host"] = node.host;
    value["port"] = node.port;
    value["healthy"] = node.healthy;
    value["weight"] = node.weight == 0 ? 100 : node.weight;
    value["zone"] = node.zone;
    return value.dump();
}

StatusOr<ClusterNode> ParseEtcdNodeValue(const std::string& node_id, const std::string& value) {
    const auto trimmed_begin = value.find_first_not_of(" \t\r\n");
    if (trimmed_begin == std::string::npos) {
        return Status::InvalidArgument("etcd node value cannot be empty");
    }

    if (value[trimmed_begin] != '{') {
        return ParseLegacyNodeValue(node_id, value);
    }

    auto parsed = nlohmann::json::parse(value, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return Status::InvalidArgument("invalid etcd node JSON value");
    }

    try {
        ClusterNode node;
        node.id = parsed.value("id", node_id);
        if (node.id.empty()) {
            node.id = node_id;
        }
        if (!parsed.contains("host") || !parsed["host"].is_string()) {
            return Status::InvalidArgument("etcd node JSON value missing string host");
        }
        if (!parsed.contains("port") || !parsed["port"].is_number_unsigned()) {
            return Status::InvalidArgument("etcd node JSON value missing numeric port");
        }

        node.host = parsed["host"].get<std::string>();
        const auto parsed_port = parsed["port"].get<unsigned int>();
        if (node.host.empty() || parsed_port == 0 || parsed_port > 65535) {
            return Status::InvalidArgument("etcd node JSON value has invalid host or port");
        }
        node.port = static_cast<std::uint16_t>(parsed_port);
        node.healthy = parsed.value("healthy", true);
        node.weight = parsed.value("weight", 100U);
        if (node.weight == 0) {
            node.weight = 100;
        }
        node.zone = parsed.value("zone", std::string{});
        return node;
    } catch (const nlohmann::json::exception& error) {
        return Status::InvalidArgument(std::string("invalid etcd node JSON field type: ") + error.what());
    }
}

#if defined(KVAI_HAVE_ETCD)

EtcdServiceDiscovery::EtcdServiceDiscovery(std::string endpoints, std::string prefix,
                                           ClusterNode local_node, std::uint32_t lease_ttl_s)
    : endpoints_(std::move(endpoints)),
      prefix_(std::move(prefix)),
      local_node_(std::move(local_node)),
      lease_ttl_s_(lease_ttl_s) {}

EtcdServiceDiscovery::~EtcdServiceDiscovery() {
    (void)Stop();
}

Status EtcdServiceDiscovery::Start(ConsistentHashRouter* router) {
    if (router == nullptr) {
        return Status::InvalidArgument("router cannot be null");
    }
    stopping_.store(false);
    router_ = router;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_ = EtcdDiscoveryStatus{};
        status_.available = true;
        status_.running = false;
        status_.state = "starting";
    }

    client_state_ = std::make_unique<EtcdClientState>();

    try {
        client_state_->client = std::make_unique<etcd::Client>(endpoints_);
    } catch (const std::exception& error) {
        RecordError("unavailable", std::string("failed to connect to etcd: ") + error.what(), true);
        return Status::Unavailable(std::string("failed to connect to etcd: ") + error.what());
    }

    // Grant a lease
    auto lease_response = client_state_->client->leasegrant(lease_ttl_s_).get();
    if (!lease_response.is_ok()) {
        RecordError("unavailable", "failed to grant etcd lease: " + lease_response.error_message(), true);
        return Status::Unavailable("failed to grant etcd lease: " + lease_response.error_message());
    }
    client_state_->lease_id = lease_response.value().lease();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.lease_id = client_state_->lease_id;
    }

    // Register local node
    const auto key = prefix_ + local_node_.id;
    const auto value = SerializeEtcdNodeValue(local_node_);
    auto set_response = client_state_->client->set(key, value, client_state_->lease_id).get();
    if (!set_response.is_ok()) {
        RecordError("unavailable", "failed to register node in etcd: " + set_response.error_message(), true);
        return Status::Unavailable("failed to register node in etcd: " + set_response.error_message());
    }

    // Start keep-alive for the lease
    client_state_->keep_alive = std::make_unique<etcd::KeepAlive>(
        *client_state_->client,
        [this](std::exception_ptr error) {
            std::string message = "unknown keepalive error";
            if (error) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& caught) {
                    message = caught.what();
                } catch (...) {
                    message = "non-standard keepalive exception";
                }
            }
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.degraded = true;
                status_.state = "degraded";
                status_.last_error = "etcd keepalive error: " + message;
                ++status_.keepalive_error_count;
            }
            log::Error("etcd-discovery", "keepalive error", {{"error", message}});
        },
        static_cast<int>(lease_ttl_s_), client_state_->lease_id);

    // Load initial node list
    auto reload_status = ReloadNodes();
    if (!reload_status.ok()) {
        log::Warn("etcd-discovery", "initial node reload failed", {{"error", reload_status.ToString()}});
    }

    // Start watch thread
    watch_thread_ = std::thread([this]() { WatchLoop(); });
    RecordSuccess("running");

    log::Info("etcd-discovery", "started etcd service discovery",
              {{"endpoints", endpoints_}, {"node_id", local_node_.id}, {"lease_ttl_s", std::to_string(lease_ttl_s_)}});

    return Status::Ok();
}

Status EtcdServiceDiscovery::Stop() {
    if (stopping_.load()) {
        return Status::Ok();
    }
    stopping_.store(true);

    // Stop watcher
    if (client_state_ && client_state_->watcher) {
        std::lock_guard<std::mutex> lock(client_mutex_);
        client_state_->watcher->Cancel();
    }

    // Stop keep-alive
    if (client_state_ && client_state_->keep_alive) {
        client_state_->keep_alive->Cancel();
    }

    // Revoke lease (auto-deletes the node key)
    if (client_state_ && client_state_->client && client_state_->lease_id != 0) {
        try {
            client_state_->client->leaserevoke(client_state_->lease_id).get();
        } catch (...) {
            // Best effort
        }
    }

    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }

    client_state_.reset();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.running = false;
        status_.state = "stopped";
    }

    log::Info("etcd-discovery", "stopped etcd service discovery", {{"node_id", local_node_.id}});
    return Status::Ok();
}

std::vector<ClusterNode> EtcdServiceDiscovery::CurrentNodes() const {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    return current_nodes_;
}

EtcdDiscoveryStatus EtcdServiceDiscovery::DiscoveryStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void EtcdServiceDiscovery::SetRingChangeCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    ring_change_callback_ = std::move(callback);
}

void EtcdServiceDiscovery::WatchLoop() {
    auto backoff = std::chrono::milliseconds(500);
    constexpr auto kMaxBackoff = std::chrono::milliseconds(5000);

    while (!stopping_.load()) {
        const auto reload_status = ReloadNodes();
        if (!reload_status.ok()) {
            RecordError("degraded", reload_status.ToString(), true);
        }

        bool normal_stop = false;
        try {
            const auto range_end = PrefixRangeEnd(prefix_);
            {
                std::lock_guard<std::mutex> lock(client_mutex_);
                if (stopping_.load()) {
                    break;
                }
                client_state_->watcher = std::make_unique<etcd::Watcher>(
                    *client_state_->client, prefix_, range_end,
                    [this](etcd::Response response) {
                        if (!response.is_ok()) {
                            if (!stopping_.load()) {
                                RecordError("degraded", "etcd watch error: " + response.error_message(), true);
                                log::Warn("etcd-discovery", "watch error", {{"error", response.error_message()}});
                            }
                            return;
                        }

                        const auto status = ReloadNodes();
                        if (!status.ok() && !stopping_.load()) {
                            RecordError("degraded", status.ToString(), true);
                            log::Warn("etcd-discovery", "node reload after watch event failed", {{"error", status.ToString()}});
                        }
                    });
            }
            normal_stop = client_state_->watcher->Wait();
        } catch (const std::exception& error) {
            if (!stopping_.load()) {
                RecordError("degraded", std::string("etcd watch loop failed: ") + error.what(), true);
                log::Error("etcd-discovery", "watch loop failed", {{"error", error.what()}});
            }
        }

        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            if (client_state_) {
                client_state_->watcher.reset();
            }
        }

        if (stopping_.load()) {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            ++status_.watch_restart_count;
            status_.degraded = true;
            status_.state = normal_stop ? "watch_restarting" : "watch_reconnecting";
            if (status_.last_error.empty()) {
                status_.last_error = normal_stop ? "etcd watch stopped" : "etcd watch disconnected";
            }
        }
        log::Warn("etcd-discovery", "restarting etcd watch",
                  {{"normal_stop", normal_stop ? "true" : "false"}, {"backoff_ms", std::to_string(backoff.count())}});

        const auto deadline = std::chrono::steady_clock::now() + backoff;
        while (!stopping_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        backoff = std::min(backoff * 2, kMaxBackoff);
    }
}

Status EtcdServiceDiscovery::ReloadNodes() {
    if (!client_state_ || !client_state_->client) {
        return Status::Unavailable("etcd client is not initialized");
    }

    auto list_response = client_state_->client->ls(prefix_).get();
    if (!list_response.is_ok()) {
        return Status::Unavailable("failed to list etcd prefix " + prefix_ + ": " + list_response.error_message());
    }

    std::vector<ClusterNode> nodes;
    for (std::size_t index = 0; index < list_response.keys().size(); ++index) {
        const auto& node_key = list_response.keys()[index];
        if (node_key.size() < prefix_.size() || node_key.compare(0, prefix_.size(), prefix_) != 0) {
            continue;
        }
        const auto& node_value = list_response.values()[index].as_string();
        std::string node_id = node_key.substr(prefix_.size());
        auto node = ParseEtcdNodeValue(node_id, node_value);
        if (!node.ok()) {
            log::Warn("etcd-discovery", "skipping invalid node value",
                      {{"key", node_key}, {"error", node.status().ToString()}});
            continue;
        }
        node.value().healthy = true;
        nodes.push_back(std::move(node.value()));
    }
    RebuildRing(nodes);
    RecordSuccess("running");
    return Status::Ok();
}

void EtcdServiceDiscovery::RebuildRing(const std::vector<ClusterNode>& nodes) {
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        current_nodes_ = nodes;
    }
    if (router_ != nullptr) {
        router_->Rebuild(nodes);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            ++status_.ring_rebuild_count;
            status_.known_node_count = nodes.size();
        }
        log::Info("etcd-discovery", "rebuilt slot owner ring",
                  {{"node_count", std::to_string(nodes.size())},
                   {"remote_forwarding_enabled", "false"}});
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = ring_change_callback_;
        }
        if (callback) {
            callback();
        }
    }
}

void EtcdServiceDiscovery::RecordSuccess(std::string state) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.available = true;
    status_.running = true;
    status_.degraded = status_.keepalive_error_count > 0;
    status_.state = std::move(state);
    if (!status_.degraded) {
        status_.last_error.clear();
    }
    status_.last_success_unix_ms = UnixMillis();
}

void EtcdServiceDiscovery::RecordError(std::string state, std::string message, bool degraded) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.available = true;
    status_.running = true;
    status_.degraded = degraded;
    status_.state = std::move(state);
    status_.last_error = std::move(message);
}

#else  // !KVAI_HAVE_ETCD

// Fallback implementation when etcd-cpp-apiv3 is not available

struct EtcdServiceDiscovery::EtcdClientState {};

EtcdServiceDiscovery::EtcdServiceDiscovery(std::string endpoints, std::string prefix,
                                           ClusterNode local_node, std::uint32_t lease_ttl_s)
    : endpoints_(std::move(endpoints)),
      prefix_(std::move(prefix)),
      local_node_(std::move(local_node)),
      lease_ttl_s_(lease_ttl_s) {}

EtcdServiceDiscovery::~EtcdServiceDiscovery() = default;

Status EtcdServiceDiscovery::Start(ConsistentHashRouter* /*router*/) {
    RecordError("unavailable", "etcd service discovery is not available in this build (etcd-cpp-apiv3 not found)", false);
    return Status::NotSupported("etcd service discovery is not available in this build (etcd-cpp-apiv3 not found)");
}

Status EtcdServiceDiscovery::Stop() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.running = false;
    status_.state = "stopped";
    return Status::Ok();
}

std::vector<ClusterNode> EtcdServiceDiscovery::CurrentNodes() const {
    return {};
}

EtcdDiscoveryStatus EtcdServiceDiscovery::DiscoveryStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    EtcdDiscoveryStatus status = status_;
    status.available = false;
    if (status.state == "unavailable") {
        status.last_error = "etcd service discovery is not available in this build";
    }
    return status;
}

void EtcdServiceDiscovery::SetRingChangeCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    ring_change_callback_ = std::move(callback);
}

Status EtcdServiceDiscovery::ReloadNodes() {
    return Status::NotSupported("etcd service discovery is not available in this build (etcd-cpp-apiv3 not found)");
}

void EtcdServiceDiscovery::RebuildRing(const std::vector<ClusterNode>& /*nodes*/) {}

void EtcdServiceDiscovery::RecordSuccess(std::string state) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = std::move(state);
}

void EtcdServiceDiscovery::RecordError(std::string state, std::string message, bool degraded) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = std::move(state);
    status_.last_error = std::move(message);
    status_.degraded = degraded;
}

#endif  // KVAI_HAVE_ETCD

}  // namespace kvai::infra
