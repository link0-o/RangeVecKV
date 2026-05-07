#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "gateway/http_runtime.h"
#include "gateway/server.h"
#include "infra/config.h"
#include "infra/logging.h"

namespace {

void PrintUsage() {
    std::cout << "Usage: kvai_server --config <path> [--query <text>] [--top-k <n>] [--collection <name>] [--healthcheck] [--dump-openapi] [--serve]\n";
}

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    for (char ch : value) {
        switch (ch) {
        case '\\':
        case '"':
            escaped.push_back('\\');
            escaped.push_back(ch);
            break;
        case '\n':
            escaped += "\\n";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::atomic<bool> g_stop_requested{false};

void HandleSignal(int) {
    g_stop_requested.store(true);
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/server.yaml";
    std::string query_text;
    std::string collection;
    std::size_t top_k = 5;
    bool healthcheck = false;
    bool dump_openapi = false;
    bool serve = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
        } else if (argument == "--query" && index + 1 < argc) {
            query_text = argv[++index];
        } else if (argument == "--top-k" && index + 1 < argc) {
            top_k = static_cast<std::size_t>(std::stoul(argv[++index]));
        } else if (argument == "--collection" && index + 1 < argc) {
            collection = argv[++index];
        } else if (argument == "--healthcheck") {
            healthcheck = true;
        } else if (argument == "--dump-openapi") {
            dump_openapi = true;
        } else if (argument == "--serve") {
            serve = true;
        } else {
            PrintUsage();
            return EXIT_FAILURE;
        }
    }

    auto config = kvai::infra::ConfigLoader::LoadFromFile(config_path);
    if (!config.ok()) {
        std::cerr << config.status().ToString() << std::endl;
        return EXIT_FAILURE;
    }

    if (config.value().log_level == "debug") {
        kvai::infra::Logger::Instance().SetLevel(kvai::infra::LogLevel::kDebug);
    }

    kvai::gateway::InProcessGatewayServer server(config.value());
    auto start_status = server.Start();
    if (!start_status.ok()) {
        std::cerr << start_status.ToString() << std::endl;
        return EXIT_FAILURE;
    }

    if (healthcheck) {
        const auto report = server.HealthCheck("");
        std::cout << "{\n"
                  << "  \"trace_id\": \"" << JsonEscape(report.trace_id) << "\",\n"
                  << "  \"status\": \"" << JsonEscape(report.status) << "\",\n"
                  << "  \"version\": \"" << JsonEscape(report.version) << "\"\n"
                  << "}\n";
        server.Stop();
        return EXIT_SUCCESS;
    }

    if (dump_openapi) {
        std::cout << server.OpenApiSpec();
        server.Stop();
        return EXIT_SUCCESS;
    }

    if (!query_text.empty()) {
        kvai::gateway::SemanticSearchQuery query;
        query.collection = collection;
        query.query = query_text;
        query.top_k = top_k;

        auto result = server.Search(query);
        if (!result.ok()) {
            std::cerr << result.status().ToString() << std::endl;
            server.Stop();
            return EXIT_FAILURE;
        }

        std::cout << "{\n"
                  << "  \"trace_id\": \"" << JsonEscape(result.value().trace_id) << "\",\n"
                  << "  \"degraded\": " << (result.value().degraded ? "true" : "false") << ",\n"
                  << "  \"message\": \"" << JsonEscape(result.value().message) << "\",\n"
                  << "  \"hits\": [\n";

        for (std::size_t index = 0; index < result.value().hits.size(); ++index) {
            const auto& hit = result.value().hits[index];
            std::cout << "    {\"key\": \"" << JsonEscape(hit.key) << "\", \"title\": \"" << JsonEscape(hit.title)
                      << "\", \"snippet\": \"" << JsonEscape(hit.snippet) << "\", \"score\": " << hit.score << "}";
            if (index + 1 < result.value().hits.size()) {
                std::cout << ',';
            }
            std::cout << '\n';
        }

        std::cout << "  ]\n}\n";
        server.Stop();
        return EXIT_SUCCESS;
    }

    server.Stop();

    if (!serve && query_text.empty() && !healthcheck && !dump_openapi) {
        serve = true;
    }

    if (serve) {
        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);

        kvai::gateway::HttpGatewayRuntime runtime(config.value());
        auto runtime_status = runtime.Start();
        if (!runtime_status.ok()) {
            std::cerr << runtime_status.ToString() << std::endl;
            return EXIT_FAILURE;
        }

        std::cout << "kvai_server serving on http://" << config.value().host << ':' << config.value().port << "\n";
        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        runtime.Stop();
    }

    return EXIT_SUCCESS;
}