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

}  // namespace

int main() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "rangeveckv-http-test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    kvai::infra::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 18080;
    config.require_api_key = true;
    config.api_key = "secret-token";
    config.wal_path = (temp_dir / "gateway.wal").string();
    config.snapshot_path = (temp_dir / "gateway.snapshot").string();
    config.index_path = (temp_dir / "gateway.index").string();

    kvai::gateway::HttpGatewayRuntime runtime(config);
    if (!Expect(runtime.Start().ok(), "http runtime start failed")) {
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