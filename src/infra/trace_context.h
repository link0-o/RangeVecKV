#pragma once

#include <string>

namespace kvai::infra {

class TraceContext {
public:
    static std::string GenerateTraceId();
};

}  // namespace kvai::infra