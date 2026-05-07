#pragma once

#include <string>

#include "infra/status.h"

namespace kvai::gateway {

class BrpcGatewayServerAdapter {
public:
    kvai::infra::Status Start(const std::string& endpoint);
    kvai::infra::Status Stop();
};

}  // namespace kvai::gateway