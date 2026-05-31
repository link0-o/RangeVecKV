#include "infra/etcd_discovery.h"

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

namespace {

std::string SerializeNode(const ClusterNode& node) {
    return node.host + ":" + std::to_string(node.port);
}

ClusterNode ParseNode(const std::string& node_id, const std::string& value) {
    ClusterNode node;
    node.id = node_id;
    const auto colon = value.rfind(':');
    if (colon != std::string::npos) {
        node.host = value.substr(0, colon);
        node.port = static_cast<std::uint16_t>(std::stoul(value.substr(colon + 1)));
    } else {
        node.host = value;
    }
    return node;
}

}  // namespace

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
    router_ = router;

    client_state_ = std::make_unique<EtcdClientState>();

    try {
        client_state_->client = std::make_unique<etcd::Client>(endpoints_);
    } catch (const std::exception& error) {
        return Status::Unavailable(std::string("failed to connect to etcd: ") + error.what());
    }

    // Grant a lease
    auto lease_response = client_state_->client->leasegrant(lease_ttl_s_).get();
    if (!lease_response.is_ok()) {
        return Status::Unavailable("failed to grant etcd lease: " + lease_response.error_message());
    }
    client_state_->lease_id = lease_response.value().lease();

    // Register local node
    const auto key = prefix_ + local_node_.id;
    const auto value = SerializeNode(local_node_);
    auto set_response = client_state_->client->set(key, value, client_state_->lease_id).get();
    if (!set_response.is_ok()) {
        return Status::Unavailable("failed to register node in etcd: " + set_response.error_message());
    }

    // Start keep-alive for the lease
    client_state_->keep_alive = std::make_unique<etcd::KeepAlive>(
        *client_state_->client, lease_ttl_s_, client_state_->lease_id);

    // Load initial node list
    auto list_response = client_state_->client->ls(prefix_).get();
    if (list_response.is_ok()) {
        std::vector<ClusterNode> nodes;
        for (std::size_t index = 0; index < list_response.keys().size(); ++index) {
            const auto& node_key = list_response.keys()[index];
            const auto& node_value = list_response.values()[index].as_string();
            // Extract node ID from the key (strip prefix)
            std::string node_id = node_key.substr(prefix_.size());
            auto node = ParseNode(node_id, node_value);
            node.healthy = true;
            nodes.push_back(std::move(node));
        }
        RebuildRing(nodes);
    }

    // Start watch thread
    stopping_.store(false);
    watch_thread_ = std::thread([this]() { WatchLoop(); });

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

    log::Info("etcd-discovery", "stopped etcd service discovery", {{"node_id", local_node_.id}});
    return Status::Ok();
}

std::vector<ClusterNode> EtcdServiceDiscovery::CurrentNodes() const {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    return current_nodes_;
}

void EtcdServiceDiscovery::WatchLoop() {
    try {
        client_state_->watcher = std::make_unique<etcd::Watcher>(
            *client_state_->client, prefix_,
            [this](etcd::Response response) {
                if (!response.is_ok()) {
                    if (!stopping_.load()) {
                        log::Warn("etcd-discovery", "watch error", {{"error", response.error_message()}});
                    }
                    return;
                }

                // Re-list all nodes under the prefix
                auto list_response = client_state_->client->ls(prefix_).get();
                if (list_response.is_ok()) {
                    std::vector<ClusterNode> nodes;
                    for (std::size_t index = 0; index < list_response.keys().size(); ++index) {
                        const auto& node_key = list_response.keys()[index];
                        const auto& node_value = list_response.values()[index].as_string();
                        std::string node_id = node_key.substr(prefix_.size());
                        auto node = ParseNode(node_id, node_value);
                        node.healthy = true;
                        nodes.push_back(std::move(node));
                    }
                    RebuildRing(nodes);
                }
            });
    } catch (const std::exception& error) {
        if (!stopping_.load()) {
            log::Error("etcd-discovery", "watch loop failed", {{"error", error.what()}});
        }
    }
}

void EtcdServiceDiscovery::RebuildRing(const std::vector<ClusterNode>& nodes) {
    {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        current_nodes_ = nodes;
    }
    if (router_ != nullptr) {
        router_->Rebuild(nodes);
        log::Info("etcd-discovery", "rebuilt consistent hash ring",
                  {{"node_count", std::to_string(nodes.size())}});
    }
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
    return Status::NotSupported("etcd service discovery is not available in this build (etcd-cpp-apiv3 not found)");
}

Status EtcdServiceDiscovery::Stop() {
    return Status::Ok();
}

std::vector<ClusterNode> EtcdServiceDiscovery::CurrentNodes() const {
    return {};
}

#endif  // KVAI_HAVE_ETCD

}  // namespace kvai::infra
