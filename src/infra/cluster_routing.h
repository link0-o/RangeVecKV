#pragma once

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

#include "infra/status.h"

namespace kvai::infra {

struct ClusterNode {
    std::string id;
    std::string host;
    std::uint16_t port = 0;
    bool healthy = true;
    std::uint32_t weight = 100;
    std::string zone;
};

struct RouteDecision {
    ClusterNode primary;
    std::vector<ClusterNode> replicas;
    bool local_owner = false;
    bool has_primary = false;
};

StatusOr<std::vector<ClusterNode>> ParseStaticClusterNodes(const std::string& encoded);

class ConsistentHashRouter {
public:
    explicit ConsistentHashRouter(std::string local_node_id, std::size_t virtual_nodes = 64);

    void Rebuild(const std::vector<ClusterNode>& nodes);
    [[nodiscard]] RouteDecision Route(const std::string& collection, const std::string& key, std::size_t replication_factor) const;
    [[nodiscard]] std::size_t NodeCount() const;

private:
    [[nodiscard]] static std::uint64_t HashValue(const std::string& value);

    std::string local_node_id_;
    std::size_t virtual_nodes_;
    mutable std::shared_mutex mutex_;
    std::vector<ClusterNode> nodes_;
    std::map<std::uint64_t, std::size_t> ring_;
};

}  // namespace kvai::infra
