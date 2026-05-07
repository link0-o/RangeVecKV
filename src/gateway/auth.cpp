#include "gateway/auth.h"

namespace kvai::gateway {

ApiKeyAuthenticator::ApiKeyAuthenticator(std::string api_key, bool required) : api_key_(std::move(api_key)), required_(required) {}

kvai::infra::Status ApiKeyAuthenticator::Authenticate(const std::map<std::string, std::string>& headers) const {
    if (!required_) {
        return kvai::infra::Status::Ok();
    }

    const auto iterator = headers.find("x-api-key");
    if (iterator == headers.end() || iterator->second.empty()) {
        return kvai::infra::Status::Unavailable("missing api key");
    }
    if (iterator->second != api_key_) {
        return kvai::infra::Status::Unavailable("invalid api key");
    }
    return kvai::infra::Status::Ok();
}

bool ApiKeyAuthenticator::enabled() const {
    return required_;
}

}  // namespace kvai::gateway