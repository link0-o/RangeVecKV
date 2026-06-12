#pragma once

#include <memory>
#include <string>

#include <brpc/server.h>

#include "gateway/auth.h"
#include "gateway/server.h"
#include "infra/config.h"
#include "infra/status.h"

namespace kvai::gateway {

class BrpcHttpServiceImpl;
class BrpcKvWriteServiceImpl;

/// BRPC-based gateway runtime that provides both HTTP REST and RPC services.
/// Replaces HttpGatewayRuntime when BRPC is available.
class BrpcGatewayRuntime {
public:
    explicit BrpcGatewayRuntime(kvai::infra::ServerConfig config);
    ~BrpcGatewayRuntime();

    kvai::infra::Status Start();
    kvai::infra::Status Stop();
    void Wait();

private:
    kvai::infra::ServerConfig config_;
    ApiKeyAuthenticator authenticator_;
    InProcessGatewayServer server_;
    BrpcHttpServiceImpl* http_service_ = nullptr;  // owned by brpc_server_
    BrpcKvWriteServiceImpl* kv_write_service_ = nullptr;  // owned by brpc_server_
    std::unique_ptr<brpc::Server> brpc_server_;
    bool started_ = false;
};

}  // namespace kvai::gateway
