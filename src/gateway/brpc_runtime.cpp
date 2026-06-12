#include "gateway/brpc_runtime.h"

#include <brpc/server.h>

#include <sstream>

#include "gateway/brpc_http_service.h"
#include "gateway/brpc_kv_write_service.h"
#include "infra/logging.h"

namespace kvai::gateway {

BrpcGatewayRuntime::BrpcGatewayRuntime(kvai::infra::ServerConfig config)
    : config_(std::move(config)),
      authenticator_(config_.api_key, config_.require_api_key),
      server_(config_) {}

BrpcGatewayRuntime::~BrpcGatewayRuntime() {
    (void)Stop();
}

kvai::infra::Status BrpcGatewayRuntime::Start() {
    if (started_) {
        return kvai::infra::Status::Ok();
    }

    auto status = server_.Start();
    if (!status.ok()) {
        return status;
    }

    brpc_server_ = std::make_unique<brpc::Server>();

    // Register the HTTP REST service with URL mappings
    http_service_ = new BrpcHttpServiceImpl(server_, authenticator_);
    if (brpc_server_->AddService(http_service_,
                                  brpc::SERVER_OWNS_SERVICE,
                                  "/healthz => Healthz,"
                                  "/metrics => Metrics,"
                                  "/openapi.yaml => OpenApi,"
                                  "/v1/search => Search,"
                                  "/v1/router => Router,"
                                  "/v1/kv => Kv,"
                                  "/v1/kv/batch => Kv,"
                                  "/v1/documents => UpsertDocument,"
                                  "/v1/documents/delete => DeleteDocument,"
                                  "/internal/migration/records => MigrateRecord") != 0) {
        return kvai::infra::Status::Internal("failed to add BRPC HTTP service");
    }

    kv_write_service_ = new BrpcKvWriteServiceImpl(server_, authenticator_);
    if (brpc_server_->AddService(kv_write_service_, brpc::SERVER_OWNS_SERVICE) != 0) {
        return kvai::infra::Status::Internal("failed to add BRPC KV write service");
    }

    // Configure server options
    brpc::ServerOptions options;
    options.num_threads = static_cast<int>(config_.worker_threads);
    options.idle_timeout_sec = 30;

    // Start the server
    std::ostringstream endpoint;
    endpoint << config_.host << ":" << config_.port;
    if (brpc_server_->Start(endpoint.str().c_str(), &options) != 0) {
        return kvai::infra::Status::Internal("failed to start BRPC server on " + endpoint.str());
    }

    started_ = true;
    kvai::infra::log::Info("brpc-runtime",
                           "BRPC gateway listening",
                           {{"host", config_.host}, {"port", std::to_string(config_.port)}});

    return kvai::infra::Status::Ok();
}

kvai::infra::Status BrpcGatewayRuntime::Stop() {
    if (!started_) {
        return kvai::infra::Status::Ok();
    }

    if (brpc_server_) {
        brpc_server_->Stop(0);
        brpc_server_->Join();
    }

    auto status = server_.Stop();
    started_ = false;
    return status;
}

void BrpcGatewayRuntime::Wait() {
    if (brpc_server_) {
        brpc_server_->RunUntilAskedToQuit();
    }
}

}  // namespace kvai::gateway
