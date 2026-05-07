#include "gateway/brpc_service_stub.h"

namespace kvai::gateway {

kvai::infra::Status BrpcGatewayServerAdapter::Start(const std::string& endpoint) {
    return kvai::infra::Status::NotSupported("BRPC adapter is not compiled in for endpoint: " + endpoint);
}

kvai::infra::Status BrpcGatewayServerAdapter::Stop() {
    return kvai::infra::Status::Ok();
}

}  // namespace kvai::gateway