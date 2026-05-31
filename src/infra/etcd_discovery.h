#pragma once

#include <atomic>
#include <cstdint>
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

private:
    void WatchLoop();
    void RefreshLease();
    void RebuildRing(const std::vector<ClusterNode>& nodes);

    std::string endpoints_;
    std::string prefix_;
    ClusterNode local_node_;
    std::uint32_t lease_ttl_s_;

    ConsistentHashRouter* router_ = nullptr;
    mutable std::mutex nodes_mutex_;
    std::vector<ClusterNode> current_nodes_;

    std::atomic<bool> stopping_{false};
    std::thread watch_thread_;
    std::thread keepalive_thread_;

    // Opaque pointer to etcd client (implementation detail in .cpp)
    struct EtcdClientState;
    std::unique_ptr<EtcdClientState> client_state_;
};

}  // namespace kvai::infra
