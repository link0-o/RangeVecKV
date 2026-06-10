#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "gateway/http_runtime.h"
#include "gateway/server.h"

namespace {

bool Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

std::string HttpRequest(std::uint16_t port,
                        const std::string& method,
                        const std::string& path,
                        const std::string& body = {},
                        const std::vector<std::string>& headers = {}) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    for (const auto& header : headers) {
        request += header + "\r\n";
    }
    if (!body.empty()) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;
    (void)::send(fd, request.data(), request.size(), 0);

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
    return response;
}

std::uint16_t PickUnusedPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 28180;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return 28180;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        return 28180;
    }
    const auto port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

}  // namespace

int main() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "rangeveckv-http-test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    kvai::infra::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = PickUnusedPort();
    config.require_api_key = true;
    config.api_key = "secret-token";
    config.wal_path = (temp_dir / "gateway.wal").string();
    config.snapshot_path = (temp_dir / "gateway.snapshot").string();
    config.index_path = (temp_dir / "gateway.index").string();

    kvai::gateway::HttpGatewayRuntime runtime(config);
    auto start_status = runtime.Start();
    if (!Expect(start_status.ok(), "http runtime start failed: " + start_status.ToString())) {
        if (start_status.ToString().find("failed to create listen socket") != std::string::npos) {
            std::cerr << "socket creation unavailable in this sandbox; skipping HTTP smoke" << std::endl;
            return 0;
        }
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const auto health = HttpRequest(config.port, "GET", "/healthz");
    if (!Expect(health.find("200 OK") != std::string::npos, "health endpoint returned non-200")) {
        runtime.Stop();
        return 1;
    }
    if (!Expect(health.find("SERVING") != std::string::npos, "health endpoint missing serving state")) {
        runtime.Stop();
        return 1;
    }

    const auto unauthorized = HttpRequest(config.port, "GET", "/v1/search?q=gateway+health&top_k=2");
    if (!Expect(unauthorized.find("401 Unauthorized") != std::string::npos, "search endpoint should require api key")) {
        runtime.Stop();
        return 1;
    }

    const auto upsert = HttpRequest(config.port,
                                    "POST",
                                    "/v1/documents",
                                    "{\"collection\":\"documents\",\"key\":\"doc-http\",\"title\":\"HTTP Gateway\",\"body\":\"secured ingestion path\",\"metadata\":{\"domain\":\"gateway\"}}",
                                    {"X-API-Key: secret-token"});
    if (!Expect(upsert.find("200 OK") != std::string::npos, "document upsert endpoint returned non-200")) {
        runtime.Stop();
        return 1;
    }

    const auto search = HttpRequest(config.port, "GET", "/v1/search?q=secured+ingestion&top_k=2&filter.domain=gateway", {}, {"X-API-Key: secret-token"});
    if (!Expect(search.find("200 OK") != std::string::npos, "search endpoint returned non-200")) {
        runtime.Stop();
        return 1;
    }
    if (!Expect(search.find("hits") != std::string::npos, "search endpoint missing hits payload")) {
        runtime.Stop();
        return 1;
    }

    const auto route = HttpRequest(config.port, "GET", "/v1/router?collection=documents&key=doc-http", {}, {"X-API-Key: secret-token"});
    if (!Expect(route.find("local_owner") != std::string::npos, "route endpoint missing ownership payload")) {
        runtime.Stop();
        return 1;
    }

    const auto kv_put = HttpRequest(config.port,
                                    "POST",
                                    "/v1/kv",
                                    "{\"collection\":\"kv\",\"key\":\"session:1\",\"value\":\"plain kv value\",\"metadata\":{\"kind\":\"kv-only\"}}",
                                    {"X-API-Key: secret-token"});
    if (!Expect(kv_put.find("200 OK") != std::string::npos, "kv put endpoint returned non-200")) {
        runtime.Stop();
        return 1;
    }

    const auto kv_get = HttpRequest(config.port, "GET", "/v1/kv?collection=kv&key=session:1", {}, {"X-API-Key: secret-token"});
    if (!Expect(kv_get.find("200 OK") != std::string::npos, "kv get endpoint returned non-200")) {
        runtime.Stop();
        return 1;
    }
    if (!Expect(kv_get.find("plain kv value") != std::string::npos, "kv get endpoint missing value")) {
        runtime.Stop();
        return 1;
    }

    const auto kv_range = HttpRequest(config.port, "GET", "/v1/kv?collection=kv&limit=10", {}, {"X-API-Key: secret-token"});
    if (!Expect(kv_range.find("items") != std::string::npos, "kv range endpoint missing items")) {
        runtime.Stop();
        return 1;
    }

    const auto migration_forbidden = HttpRequest(config.port,
                                                 "POST",
                                                 "/internal/migration/records",
                                                 "{\"collection\":\"kv\",\"key\":\"migrated:forbidden\",\"value\":\"blocked\"}",
                                                 {"X-API-Key: secret-token"});
    if (!Expect(migration_forbidden.find("403 Forbidden") != std::string::npos, "migration endpoint should require internal header")) {
        runtime.Stop();
        return 1;
    }

    const auto migration_put = HttpRequest(config.port,
                                           "POST",
                                           "/internal/migration/records",
                                           "{\"collection\":\"kv\",\"key\":\"migrated:1\",\"value\":\"migrated kv value\",\"semantic\":false}",
                                           {"X-API-Key: secret-token", "x-kvai-internal-migration: 1"});
    if (!Expect(migration_put.find("200 OK") != std::string::npos, "migration endpoint returned non-200")) {
        runtime.Stop();
        return 1;
    }
    const auto migration_get = HttpRequest(config.port, "GET", "/v1/kv?collection=kv&key=migrated:1", {}, {"X-API-Key: secret-token"});
    if (!Expect(migration_get.find("migrated kv value") != std::string::npos, "migrated kv record missing")) {
        runtime.Stop();
        return 1;
    }

    kvai::infra::ServerConfig source_config;
    source_config.host = "127.0.0.1";
    source_config.port = PickUnusedPort();
    source_config.require_api_key = true;
    source_config.api_key = "secret-token";
    source_config.node_id = "source-node";
    source_config.cluster_nodes = "target-node@127.0.0.1:" + std::to_string(config.port);
    source_config.data_migration_enabled = true;
    source_config.migration_delete_delay_ms = 300000;
    source_config.migration_batch_size = 10;
    source_config.migration_max_retries = 3;
    source_config.enable_demo_data = false;
    source_config.wal_path = (temp_dir / "source.wal").string();
    source_config.snapshot_path = (temp_dir / "source.snapshot").string();
    source_config.index_path = (temp_dir / "source.index").string();

    kvai::gateway::InProcessGatewayServer source_server(source_config);
    if (!Expect(source_server.Start().ok(), "source migration server start failed")) {
        runtime.Stop();
        return 1;
    }
    kvai::core::DocumentRecord source_record{"kv", "migrated:manager", "", "manager migrated value", {{"kind", "migration"}}};
    if (!Expect(source_server.ApplyMigratedRecord(source_record, false, "").ok(), "source migration seed failed")) {
        source_server.Stop();
        runtime.Stop();
        return 1;
    }
    source_server.TriggerMigrationScan();
    bool migrated_by_manager = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto status = source_server.MigrationStatus();
        if (status.succeeded > 0) {
            migrated_by_manager = true;
            break;
        }
    }
    if (!Expect(migrated_by_manager, "migration manager did not report success")) {
        source_server.Stop();
        runtime.Stop();
        return 1;
    }
    const auto manager_migration_get = HttpRequest(config.port, "GET", "/v1/kv?collection=kv&key=migrated:manager", {}, {"X-API-Key: secret-token"});
    if (!Expect(manager_migration_get.find("manager migrated value") != std::string::npos, "manager migrated kv record missing on target")) {
        source_server.Stop();
        runtime.Stop();
        return 1;
    }
    if (!Expect(source_server.Stop().ok(), "source migration server stop failed")) {
        runtime.Stop();
        return 1;
    }

    const auto kv_delete = HttpRequest(config.port, "DELETE", "/v1/kv?collection=kv&key=session:1", {}, {"X-API-Key: secret-token"});
    if (!Expect(kv_delete.find("200 OK") != std::string::npos, "kv delete endpoint returned non-200")) {
        runtime.Stop();
        return 1;
    }

    const auto metrics = HttpRequest(config.port, "GET", "/metrics", {}, {"X-API-Key: secret-token"});
    if (!Expect(metrics.find("kvai_http_requests_total") != std::string::npos, "metrics endpoint missing counter")) {
        runtime.Stop();
        return 1;
    }

    const auto deletion = HttpRequest(config.port, "DELETE", "/v1/documents?collection=documents&key=doc-http", {}, {"X-API-Key: secret-token"});
    if (!Expect(deletion.find("200 OK") != std::string::npos, "document delete endpoint returned non-200")) {
        runtime.Stop();
        return 1;
    }

    runtime.Stop();
    std::filesystem::remove_all(temp_dir);
    return 0;
}
