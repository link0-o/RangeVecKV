#include "infra/cluster_routing.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <sstream>

namespace kvai::infra {

namespace {

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

}  // namespace

StatusOr<std::vector<ClusterNode>> ParseStaticClusterNodes(const std::string& encoded) {
    std::vector<ClusterNode> nodes;
    std::stringstream stream(encoded);
    std::string token;
    while (std::getline(stream, token, ',')) {
        token = Trim(std::move(token));
        if (token.empty()) {
            continue;
        }

        const auto at_pos = token.find('@');
        const auto colon_pos = token.rfind(':');
        if (at_pos == std::string::npos || colon_pos == std::string::npos || colon_pos <= at_pos + 1) {
            return Status::InvalidArgument("invalid cluster node entry: " + token);
        }

        ClusterNode node;
        node.id = token.substr(0, at_pos);
        node.host = token.substr(at_pos + 1, colon_pos - at_pos - 1);
        node.port = static_cast<std::uint16_t>(std::stoul(token.substr(colon_pos + 1)));
        if (node.id.empty() || node.host.empty() || node.port == 0) {
            return Status::InvalidArgument("invalid cluster node entry: " + token);
        }
        nodes.push_back(std::move(node));
    }

    if (nodes.empty()) {
        return Status::InvalidArgument("cluster.nodes cannot be empty");
    }
    return nodes;
}

ConsistentHashRouter::ConsistentHashRouter(std::string local_node_id, std::size_t virtual_nodes, std::size_t slot_count)
    : local_node_id_(std::move(local_node_id)),
      virtual_nodes_(std::max<std::size_t>(1, virtual_nodes)),
      slot_count_(std::max<std::size_t>(1, slot_count)) {}

void ConsistentHashRouter::Rebuild(const std::vector<ClusterNode>& nodes) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    nodes_ = nodes;
    ring_.clear();
    for (std::size_t node_index = 0; node_index < nodes_.size(); ++node_index) {
        for (std::size_t replica_index = 0; replica_index < virtual_nodes_; ++replica_index) {
            ring_.emplace(HashValue(nodes_[node_index].id + "#" + std::to_string(replica_index)), node_index);
        }
    }
}

RouteDecision ConsistentHashRouter::Route(const std::string& collection, const std::string& key, std::size_t replication_factor) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    RouteDecision decision;
    if (ring_.empty() || nodes_.empty()) {
        return decision;
    }

    decision.slot_id = SlotFor(collection, key);
    auto iterator = ring_.lower_bound(HashValue("slot:" + std::to_string(decision.slot_id)));
    if (iterator == ring_.end()) {
        iterator = ring_.begin();
    }

    std::vector<std::size_t> selected_indexes;
    const auto desired_replicas = std::min<std::size_t>(std::max<std::size_t>(1, replication_factor), nodes_.size());
    while (selected_indexes.size() < desired_replicas) {
        const auto node_index = iterator->second;
        if (std::find(selected_indexes.begin(), selected_indexes.end(), node_index) == selected_indexes.end()) {
            selected_indexes.push_back(node_index);
        }
        ++iterator;
        if (iterator == ring_.end()) {
            iterator = ring_.begin();
        }
    }

    decision.primary = nodes_[selected_indexes.front()];
    decision.has_primary = true;
    decision.local_owner = decision.primary.id == local_node_id_;
    for (std::size_t node_index : selected_indexes) {
        decision.replicas.push_back(nodes_[node_index]);
    }
    return decision;
}

std::size_t ConsistentHashRouter::NodeCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return nodes_.size();
}

std::size_t ConsistentHashRouter::SlotCount() const {
    return slot_count_;
}

std::uint64_t ConsistentHashRouter::HashValue(const std::string& value) {
    return static_cast<std::uint64_t>(std::hash<std::string>{}(value));
}

std::uint32_t ConsistentHashRouter::SlotFor(const std::string& collection, const std::string& key) const {
    return static_cast<std::uint32_t>(HashValue(collection + ":" + key) % slot_count_);
}

}  // namespace kvai::infra
