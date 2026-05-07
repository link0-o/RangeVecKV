#pragma once

#include <map>
#include <string>

#include "infra/status.h"

namespace kvai::gateway {

class ApiKeyAuthenticator {
public:
    ApiKeyAuthenticator(std::string api_key, bool required);

    [[nodiscard]] kvai::infra::Status Authenticate(const std::map<std::string, std::string>& headers) const;
    [[nodiscard]] bool enabled() const;

private:
    std::string api_key_;
    bool required_;
};

}  // namespace kvai::gateway