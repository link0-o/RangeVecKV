#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "gateway/json_helpers.h"
#include "gateway/server.h"
#include "infra/config.h"
#include "infra/logging.h"

#if defined(KVAI_HAVE_BRPC)
#include <brpc/controller.h>

#include "gateway/brpc_runtime.h"
#else
#include "gateway/http_runtime.h"
#endif

namespace {

void PrintUsage() {
    std::cout << "Usage: kvai_server --config <path> [--query <text>] [--top-k <n>] [--collection <name>] [--healthcheck] [--dump-openapi] [--reindex] [--serve]\n";
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
    bool reindex = false;
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
        } else if (argument == "--reindex") {
            reindex = true;
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

    kvai::infra::log::ConfigureLogger(config.value());

    const bool has_one_shot_action = healthcheck || dump_openapi || reindex || !query_text.empty();
    if (!serve && !has_one_shot_action) {
        serve = true;
    }

    if (serve && !has_one_shot_action) {
        std::signal(SIGTERM, HandleSignal);

#if defined(KVAI_HAVE_BRPC)
        kvai::gateway::BrpcGatewayRuntime runtime(config.value());
#else
        std::signal(SIGINT, HandleSignal);
        kvai::gateway::HttpGatewayRuntime runtime(config.value());
#endif
        auto runtime_status = runtime.Start();
        if (!runtime_status.ok()) {
            std::cerr << runtime_status.ToString() << std::endl;
            return EXIT_FAILURE;
        }

        std::cout << "kvai_server serving on http://" << config.value().host << ':' << config.value().port << "\n";

#if defined(KVAI_HAVE_BRPC)
        std::atomic<bool> wait_finished{false};
        std::thread termination_watcher([&wait_finished]() {
            while (!g_stop_requested.load() && !wait_finished.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (g_stop_requested.load() && !wait_finished.load()) {
                brpc::AskToQuit();
            }
        });
        runtime.Wait();
        wait_finished.store(true);
        termination_watcher.join();
#else
        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
#endif
        runtime.Stop();
        return EXIT_SUCCESS;
    }

    kvai::gateway::InProcessGatewayServer server(config.value());
    auto start_status = server.Start();
    if (!start_status.ok()) {
        std::cerr << start_status.ToString() << std::endl;
        return EXIT_FAILURE;
    }

    if (healthcheck) {
        const auto report = server.HealthCheck("");
        std::cout << kvai::gateway::json::ToJson(report).dump(2) << std::endl;
        server.Stop();
        return EXIT_SUCCESS;
    }

    if (dump_openapi) {
        std::cout << server.OpenApiSpec();
        server.Stop();
        return EXIT_SUCCESS;
    }

    if (reindex) {
        auto status = server.ReindexDocuments(collection);
        if (!status.ok()) {
            std::cerr << status.ToString() << std::endl;
            server.Stop();
            return EXIT_FAILURE;
        }
        std::cout << "reindex completed" << std::endl;
        server.Stop();
        return EXIT_SUCCESS;
    }

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

    std::cout << kvai::gateway::json::ToJson(result.value()).dump(2) << std::endl;
    server.Stop();
    return EXIT_SUCCESS;
}
