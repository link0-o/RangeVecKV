#include "infra/status.h"

#include <sstream>

namespace kvai::infra {

namespace {

const char* StatusCodeToString(StatusCode code) {
    switch (code) {
    case StatusCode::kOk:
        return "OK";
    case StatusCode::kInvalidArgument:
        return "INVALID_ARGUMENT";
    case StatusCode::kNotFound:
        return "NOT_FOUND";
    case StatusCode::kUnavailable:
        return "UNAVAILABLE";
    case StatusCode::kTimeout:
        return "TIMEOUT";
    case StatusCode::kInternal:
        return "INTERNAL";
    case StatusCode::kNotSupported:
        return "NOT_SUPPORTED";
    }

    return "UNKNOWN";
}

}  // namespace

Status::Status() : code_(StatusCode::kOk) {}

Status::Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

Status Status::Ok() { return Status(); }

Status Status::InvalidArgument(std::string message) { return Status(StatusCode::kInvalidArgument, std::move(message)); }

Status Status::NotFound(std::string message) { return Status(StatusCode::kNotFound, std::move(message)); }

Status Status::Unavailable(std::string message) { return Status(StatusCode::kUnavailable, std::move(message)); }

Status Status::Timeout(std::string message) { return Status(StatusCode::kTimeout, std::move(message)); }

Status Status::Internal(std::string message) { return Status(StatusCode::kInternal, std::move(message)); }

Status Status::NotSupported(std::string message) { return Status(StatusCode::kNotSupported, std::move(message)); }

bool Status::ok() const { return code_ == StatusCode::kOk; }

StatusCode Status::code() const { return code_; }

const std::string& Status::message() const { return message_; }

std::string Status::ToString() const {
    if (ok()) {
        return "OK";
    }

    std::ostringstream stream;
    stream << StatusCodeToString(code_) << ": " << message_;
    return stream.str();
}

}  // namespace kvai::infra