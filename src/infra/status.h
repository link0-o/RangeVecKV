#pragma once

#include <optional>
#include <string>
#include <utility>

namespace kvai::infra {

enum class StatusCode {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kUnavailable,
    kTimeout,
    kInternal,
    kNotSupported,
};

class Status {
public:
    Status();
    Status(StatusCode code, std::string message);

    static Status Ok();
    static Status InvalidArgument(std::string message);
    static Status NotFound(std::string message);
    static Status Unavailable(std::string message);
    static Status Timeout(std::string message);
    static Status Internal(std::string message);
    static Status NotSupported(std::string message);

    [[nodiscard]] bool ok() const;
    [[nodiscard]] StatusCode code() const;
    [[nodiscard]] const std::string& message() const;
    [[nodiscard]] std::string ToString() const;

private:
    StatusCode code_;
    std::string message_;
};

template <typename T>
class StatusOr {
public:
    StatusOr(Status status) : status_(std::move(status)) {}
    StatusOr(T value) : status_(Status::Ok()), value_(std::move(value)) {}

    [[nodiscard]] bool ok() const { return status_.ok(); }
    [[nodiscard]] const Status& status() const { return status_; }
    [[nodiscard]] const T& value() const { return value_.value(); }
    [[nodiscard]] T& value() { return value_.value(); }
    [[nodiscard]] T&& ConsumeValue() { return std::move(value_.value()); }

private:
    Status status_;
    std::optional<T> value_;
};

}  // namespace kvai::infra