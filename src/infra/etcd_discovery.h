#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "infra/cluster_routing.h"
#include "infra/config.h"
#include "infra/status.h"

namespace kvai::infra {

struct EtcdDiscoveryStatus {
    bool available = false;
    bool running = false;
    bool degraded = false;
    std::string state = "unavailable";
    std::string last_error;
    std::int64_t lease_id = 0;
    std::uint64_t watch_restart_count = 0;
    std::uint64_t keepalive_error_count = 0;
    std::uint64_t ring_rebuild_count = 0;
    std::size_t known_node_count = 0;
    std::int64_t last_success_unix_ms = 0;
};

std::string SerializeEtcdNodeValue(const ClusterNode& node);
StatusOr<ClusterNode> ParseEtcdNodeValue(const std::string& node_id, const std::string& value);

/// etcd-based service discovery for dynamic cluster membership.
/// When started, registers the local node with a lease-backed key,
/// watches the prefix for changes, and rebuilds the consistent hash ring
/// when nodes join or leave.
class EtcdServiceDiscovery {
public:
    EtcdServiceDiscovery(std::string endpoints, std::string prefix,
                         ClusterNode local_node, std::uint32_t lease_ttl_s);
    ~EtcdServiceDiscovery();

    Status Start(ConsistentHashRouter* router);
    Status Stop();

    /// Returns the currently known cluster nodes (thread-safe).
    std::vector<ClusterNode> CurrentNodes() const;
    EtcdDiscoveryStatus DiscoveryStatus() const;
    void SetRingChangeCallback(std::function<void()> callback);

private:
    void WatchLoop();
    Status ReloadNodes();
    void RebuildRing(const std::vector<ClusterNode>& nodes);
    void RecordSuccess(std::string state);
    void RecordError(std::string state, std::string message, bool degraded);

    std::string endpoints_;
    std::string prefix_;
    ClusterNode local_node_;
    std::uint32_t lease_ttl_s_;

    ConsistentHashRouter* router_ = nullptr;
    mutable std::mutex nodes_mutex_;
    std::vector<ClusterNode> current_nodes_;

    std::atomic<bool> stopping_{false};
    std::thread watch_thread_;

    // Opaque pointer to etcd client (implementation detail in .cpp)
    struct EtcdClientState;
    std::unique_ptr<EtcdClientState> client_state_;
    mutable std::mutex client_mutex_;
    mutable std::mutex status_mutex_;
    EtcdDiscoveryStatus status_;
    mutable std::mutex callback_mutex_;
    std::function<void()> ring_change_callback_;
};

}  // namespace kvai::infra
