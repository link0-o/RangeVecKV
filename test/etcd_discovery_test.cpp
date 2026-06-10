#include <iostream>

#include "infra/etcd_discovery.h"

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
    int failures = 0;

    kvai::infra::ClusterNode node;
    node.id = "node-a";
    node.host = "rangeveckv-a";
    node.port = 8080;
    node.healthy = true;
    node.weight = 150;
    node.zone = "zone-a";

    const auto encoded = kvai::infra::SerializeEtcdNodeValue(node);
    auto decoded = kvai::infra::ParseEtcdNodeValue("node-a", encoded);
    if (!Expect(decoded.ok(), "json node value should parse")) {
        ++failures;
    } else {
        if (!Expect(decoded.value().id == "node-a", "json node id mismatch")) ++failures;
        if (!Expect(decoded.value().host == "rangeveckv-a", "json node host mismatch")) ++failures;
        if (!Expect(decoded.value().port == 8080, "json node port mismatch")) ++failures;
        if (!Expect(decoded.value().weight == 150, "json node weight mismatch")) ++failures;
        if (!Expect(decoded.value().zone == "zone-a", "json node zone mismatch")) ++failures;
    }

    auto legacy = kvai::infra::ParseEtcdNodeValue("node-b", "10.0.0.2:9090");
    if (!Expect(legacy.ok(), "legacy host:port node value should parse")) {
        ++failures;
    } else {
        if (!Expect(legacy.value().id == "node-b", "legacy node id mismatch")) ++failures;
        if (!Expect(legacy.value().host == "10.0.0.2", "legacy node host mismatch")) ++failures;
        if (!Expect(legacy.value().port == 9090, "legacy node port mismatch")) ++failures;
        if (!Expect(legacy.value().weight == 100, "legacy node weight default mismatch")) ++failures;
    }

    if (!Expect(!kvai::infra::ParseEtcdNodeValue("bad", R"({"port":8080})").ok(), "missing host should fail")) {
        ++failures;
    }
    if (!Expect(!kvai::infra::ParseEtcdNodeValue("bad", R"({"host":"x","port":70000})").ok(), "invalid port should fail")) {
        ++failures;
    }
    if (!Expect(!kvai::infra::ParseEtcdNodeValue("bad", "{not-json").ok(), "invalid json should fail")) {
        ++failures;
    }

    kvai::infra::EtcdServiceDiscovery discovery("http://127.0.0.1:2379", "/rangeveckv/nodes/", node, 10);
    const auto status = discovery.DiscoveryStatus();
    if (!Expect(!status.running, "new discovery should not be running")) ++failures;

    return failures == 0 ? 0 : 1;
}
