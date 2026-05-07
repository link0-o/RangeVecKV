#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infra/config.h"

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
    const auto temp_dir = std::filesystem::temp_directory_path() / "rangeveckv-config-test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto config_path = temp_dir / "server.yaml";
    {
        std::ofstream output(config_path);
        output << "gateway.require_api_key: true\n";
        output << "gateway.api_key: ${KVAI_TEST_API_KEY}\n";
        output << "cluster.nodes: ${KVAI_TEST_CLUSTER}\n";
        output << "storage.enable_demo_data: false\n";
    }

    ::setenv("KVAI_TEST_API_KEY", "integration-secret", 1);
    ::setenv("KVAI_TEST_CLUSTER", "node-a@127.0.0.1:8080", 1);

    auto config = kvai::infra::ConfigLoader::LoadFromFile(config_path.string());
    if (!Expect(config.ok(), "config loading failed")) {
        return 1;
    }
    if (!Expect(config.value().require_api_key, "api key flag not parsed")) {
        return 1;
    }
    if (!Expect(config.value().api_key == "integration-secret", "api key env expansion failed")) {
        return 1;
    }
    if (!Expect(config.value().cluster_nodes == "node-a@127.0.0.1:8080", "cluster env expansion failed")) {
        return 1;
    }
    if (!Expect(!config.value().enable_demo_data, "boolean false parsing failed")) {
        return 1;
    }

    std::filesystem::remove_all(temp_dir);
    return 0;
}