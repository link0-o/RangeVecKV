#include <iostream>
#include <thread>
#include <vector>

#include "infra/cluster_routing.h"

namespace {

bool Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    auto nodes = kvai::infra::ParseStaticClusterNodes("node-a@10.0.0.1:8080,node-b@10.0.0.2:8080,node-c@10.0.0.3:8080");
    if (!Expect(nodes.ok(), "cluster node parsing failed")) {
        return 1;
    }

    kvai::infra::ConsistentHashRouter router("node-b");
    router.Rebuild(nodes.value());

    const auto route = router.Route("documents", "doc-123", 2);
    if (!Expect(route.has_primary, "route should have a primary")) {
        return 1;
    }
    if (!Expect(route.replicas.size() == 2, "route should include two replicas")) {
        return 1;
    }
    if (!Expect(!route.primary.id.empty(), "primary node id should not be empty")) {
        return 1;
    }

    std::vector<std::thread> readers;
    for (int worker = 0; worker < 4; ++worker) {
        readers.emplace_back([&router, worker]() {
            for (int index = 0; index < 200; ++index) {
                const auto concurrent_route = router.Route("documents", "doc-" + std::to_string(worker) + "-" + std::to_string(index), 2);
                if (!concurrent_route.has_primary) {
                    std::cerr << "concurrent route missing primary" << std::endl;
                }
            }
        });
    }
    for (int rebuild = 0; rebuild < 20; ++rebuild) {
        router.Rebuild(nodes.value());
    }
    for (auto& reader : readers) {
        reader.join();
    }

    return 0;
}
