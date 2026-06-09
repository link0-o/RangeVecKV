#include "gateway/auth.h"

#include <algorithm>
#include <cctype>

namespace kvai::gateway {

namespace {

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string ExtractPresentedKey(const std::map<std::string, std::string>& headers) {
    if (const auto iterator = headers.find("authorization"); iterator != headers.end()) {
        const std::string prefix = "Bearer ";
        if (iterator->second.rfind(prefix, 0) == 0) {
            return Trim(iterator->second.substr(prefix.size()));
        }
    }
    if (const auto iterator = headers.find("x-api-key"); iterator != headers.end()) {
        return Trim(iterator->second);
    }
    return {};
}

bool ConstantTimeEquals(const std::string& lhs, const std::string& rhs) {
    const auto max_size = std::max(lhs.size(), rhs.size());
    unsigned char diff = static_cast<unsigned char>(lhs.size() ^ rhs.size());
    for (std::size_t index = 0; index < max_size; ++index) {
        const auto left = index < lhs.size() ? static_cast<unsigned char>(lhs[index]) : 0;
        const auto right = index < rhs.size() ? static_cast<unsigned char>(rhs[index]) : 0;
        diff |= static_cast<unsigned char>(left ^ right);
    }
    return diff == 0;
}

}  // namespace

ApiKeyAuthenticator::ApiKeyAuthenticator(std::string api_key, bool required) : api_key_(std::move(api_key)), required_(required) {}

kvai::infra::Status ApiKeyAuthenticator::Authenticate(const std::map<std::string, std::string>& headers) const {
    if (!required_) {
        return kvai::infra::Status::Ok();
    }
    if (api_key_.empty()) {
        return kvai::infra::Status::Unavailable("api key authentication is enabled but no key is configured");
    }

    const auto presented_key = ExtractPresentedKey(headers);
    if (presented_key.empty()) {
        return kvai::infra::Status::Unavailable("missing api key");
    }
    if (!ConstantTimeEquals(presented_key, api_key_)) {
        return kvai::infra::Status::Unavailable("invalid api key");
    }
    return kvai::infra::Status::Ok();
}

bool ApiKeyAuthenticator::enabled() const {
    return required_;
}

}  // namespace kvai::gateway
