#include "gateway/http_runtime.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/document.h"
#include "gateway/json_helpers.h"
#include "infra/logging.h"

namespace kvai::gateway {

namespace {

std::string UrlDecode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto hex = value.substr(index + 1, 2);
            decoded.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            index += 2;
            continue;
        }
        decoded.push_back(value[index]);
    }
    return decoded;
}

std::map<std::string, std::string> ParseQuery(const std::string& query_string) {
    std::map<std::string, std::string> values;
    std::stringstream stream(query_string);
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        const auto separator = pair.find('=');
        if (separator == std::string::npos) {
            values.emplace(UrlDecode(pair), "");
            continue;
        }
        values.emplace(UrlDecode(pair.substr(0, separator)), UrlDecode(pair.substr(separator + 1)));
    }
    return values;
}

std::map<std::string, std::string> ParseHeaders(std::istream& stream) {
    std::map<std::string, std::string> headers;
    std::string header_line;
    while (std::getline(stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }
        if (header_line.empty()) {
            continue;
        }
        const auto separator = header_line.find(':');
        if (separator == std::string::npos) {
            continue;
        }
        std::string key = header_line.substr(0, separator);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::string value = header_line.substr(separator + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.erase(value.begin());
        }
        headers[std::move(key)] = std::move(value);
    }
    return headers;
}

std::vector<SearchFilter> ExtractFilters(const std::map<std::string, std::string>& params) {
    std::vector<SearchFilter> filters;
    for (const auto& [key, value] : params) {
        if (key.rfind("filter.", 0) == 0) {
            filters.push_back(SearchFilter{key.substr(7), value});
        }
    }
    return filters;
}

int HttpStatusFor(const kvai::infra::Status& status) {
    switch (status.code()) {
    case kvai::infra::StatusCode::kInvalidArgument:
        return 400;
    case kvai::infra::StatusCode::kNotFound:
        return 404;
    case kvai::infra::StatusCode::kUnavailable:
        return 503;
    case kvai::infra::StatusCode::kTimeout:
        return 504;
    case kvai::infra::StatusCode::kNotSupported:
        return 501;
    case kvai::infra::StatusCode::kInternal:
        return 500;
    case kvai::infra::StatusCode::kOk:
        return 200;
    }
    return 500;
}

std::string HttpStatusText(int status_code) {
    switch (status_code) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Error";
    }
}

std::size_t ParseLimit(const std::map<std::string, std::string>& params, std::size_t fallback) {
    const auto iterator = params.find("limit");
    if (iterator == params.end() || iterator->second.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::size_t>(std::stoul(iterator->second));
    } catch (...) {
        return fallback;
    }
}

std::string ErrorResponse(const kvai::infra::Status& status) {
    nlohmann::json error_json;
    error_json["message"] = status.ToString();
    const auto code = HttpStatusFor(status);
    return json::BuildHttpResponse(code, HttpStatusText(code), "application/json", error_json.dump());
}

}  // namespace

HttpGatewayRuntime::HttpGatewayRuntime(kvai::infra::ServerConfig config)
    : config_(std::move(config)),
      authenticator_(config_.api_key, config_.require_api_key),
      server_(config_),
      http_pool_(std::max<std::size_t>(2, config_.worker_threads)) {}

HttpGatewayRuntime::~HttpGatewayRuntime() {
    (void)Stop();
}

kvai::infra::Status HttpGatewayRuntime::Start() {
    if (started_) {
        return kvai::infra::Status::Ok();
    }

    auto status = server_.Start();
    if (!status.ok()) {
        return status;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return kvai::infra::Status::Internal("failed to create listen socket");
    }

    int reuse_addr = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) != 0) {
        return kvai::infra::Status::Internal("failed to enable SO_REUSEADDR");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
        return kvai::infra::Status::InvalidArgument("server.host must be a valid IPv4 address");
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return kvai::infra::Status::Internal("failed to bind listen socket: " + std::string(std::strerror(errno)));
    }
    if (::listen(listen_fd_, 128) != 0) {
        return kvai::infra::Status::Internal("failed to listen on socket: " + std::string(std::strerror(errno)));
    }

    accept_thread_ = std::thread([this]() { AcceptLoop(); });
    started_ = true;
    kvai::infra::log::Info("http-runtime",
                           "http gateway listening",
                           {{"host", config_.host}, {"port", std::to_string(config_.port)}});
    return kvai::infra::Status::Ok();
}

kvai::infra::Status HttpGatewayRuntime::Stop() {
    if (!started_) {
        return kvai::infra::Status::Ok();
    }

    stopping_.store(true);
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    http_pool_.Shutdown();
    auto status = server_.Stop();
    started_ = false;
    WakeWaiters();
    return status;
}

void HttpGatewayRuntime::Wait() {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_condition_.wait(lock, [this]() { return stopping_.load(); });
}

void HttpGatewayRuntime::AcceptLoop() {
    while (!stopping_.load()) {
        const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (stopping_.load()) {
                return;
            }
            kvai::infra::log::Warn("http-runtime",
                                   "accept failed",
                                   {{"error", std::strerror(errno)}});
            continue;
        }

        server_.MutableMetrics().IncrementHttpRequests();
        (void)http_pool_.Submit([this, client_fd]() { HandleClient(client_fd); });
    }
}

void HttpGatewayRuntime::HandleClient(int client_fd) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const auto received = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            ::close(client_fd);
            return;
        }
        request.append(buffer, static_cast<std::size_t>(received));
        if (request.size() > 65536) {
            break;
        }
    }

    const auto header_end = request.find("\r\n\r\n");
    const auto header_block = request.substr(0, header_end);
    std::istringstream header_stream(header_block);
    std::string request_line;
    std::getline(header_stream, request_line);
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }

    std::string method;
    std::string target;
    std::string version;
    std::istringstream request_line_stream(request_line);
    request_line_stream >> method >> target >> version;

    std::size_t content_length = 0;
    const auto headers = ParseHeaders(header_stream);
    if (const auto iterator = headers.find("content-length"); iterator != headers.end()) {
        content_length = static_cast<std::size_t>(std::stoul(iterator->second));
    }

    std::string body;
    if (header_end != std::string::npos) {
        body = request.substr(header_end + 4);
    }
    while (body.size() < content_length) {
        const auto received = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        body.append(buffer, static_cast<std::size_t>(received));
    }

    std::string path = target;
    std::string query_string;
    const auto query_separator = target.find('?');
    if (query_separator != std::string::npos) {
        path = target.substr(0, query_separator);
        query_string = target.substr(query_separator + 1);
    }

    std::string response;

    const bool public_route = path == "/healthz" || path == "/openapi" || path == "/openapi.yaml";
    if (!public_route) {
        const auto auth_status = authenticator_.Authenticate(headers);
        if (!auth_status.ok()) {
            nlohmann::json error_json;
            error_json["message"] = auth_status.ToString();
            response = json::BuildHttpResponse(401, "Unauthorized", "application/json", error_json.dump());
            (void)::send(client_fd, response.data(), response.size(), 0);
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
            return;
        }
    }

    if (method == "GET" && path == "/healthz") {
        response = json::BuildHttpResponse(200, "OK", "application/json", json::ToJson(server_.HealthCheck("")).dump());
    } else if (method == "GET" && path == "/metrics") {
        response = json::BuildHttpResponse(200, "OK", "text/plain; version=0.0.4", server_.Metrics().RenderPrometheus());
    } else if (method == "GET" && (path == "/openapi" || path == "/openapi.yaml")) {
        response = json::BuildHttpResponse(200, "OK", "application/yaml", server_.OpenApiSpec());
    } else if ((method == "GET" || method == "POST") && path == "/v1/search") {
        SemanticSearchQuery query;
        if (const auto iterator = headers.find("x-trace-id"); iterator != headers.end()) {
            query.trace_id = iterator->second;
        }
        if (method == "GET") {
            const auto params = ParseQuery(query_string);
            if (const auto iterator = params.find("q"); iterator != params.end()) {
                query.query = iterator->second;
            }
            if (const auto iterator = params.find("image_path"); iterator != params.end()) {
                query.image_reference = iterator->second;
            } else if (const auto iterator = params.find("image_reference"); iterator != params.end()) {
                query.image_reference = iterator->second;
            }
            if (const auto iterator = params.find("collection"); iterator != params.end()) {
                query.collection = iterator->second;
            }
            if (const auto iterator = params.find("top_k"); iterator != params.end() && !iterator->second.empty()) {
                query.top_k = static_cast<std::size_t>(std::stoul(iterator->second));
            }
            query.filters = ExtractFilters(params);
        } else {
            try {
                auto body_json = nlohmann::json::parse(body, nullptr, false);
                if (!body_json.is_discarded()) {
                    auto parsed = json::ParseSearchQuery(body_json, true);
                    if (parsed.ok()) {
                        query.query = parsed.value().query.empty() ? query.query : parsed.value().query;
                        query.image_reference = parsed.value().image_reference.empty()
                                                    ? query.image_reference
                                                    : parsed.value().image_reference;
                        query.collection = parsed.value().collection.empty() ? query.collection : parsed.value().collection;
                        if (parsed.value().top_k != 10) {
                            query.top_k = parsed.value().top_k;
                        }
                        query.filters = parsed.value().filters;
                    }
                }
            } catch (...) {
                // Malformed JSON — proceed with defaults
            }
        }

        auto result = server_.Search(query);
        if (!result.ok()) {
            nlohmann::json error_json;
            error_json["error"] = result.status().ToString();
            response = json::BuildHttpResponse(503, "Service Unavailable", "application/json", error_json.dump());
        } else {
            response = json::BuildHttpResponse(200, "OK", "application/json", json::ToJson(result.value()).dump());
        }
    } else if (method == "GET" && path == "/v1/router") {
        const auto params = ParseQuery(query_string);
        const auto collection = params.count("collection") == 0 ? std::string() : params.at("collection");
        const auto key = params.count("key") == 0 ? std::string() : params.at("key");
        response = json::BuildHttpResponse(200, "OK", "application/json", json::ToJson(server_.DescribeRoute(collection, key)).dump());
    } else if (path == "/v1/kv") {
        const auto params = ParseQuery(query_string);
        if (method == "GET") {
            const auto collection = params.count("collection") == 0 ? std::string() : params.at("collection");
            const auto key = params.count("key") == 0 ? std::string() : params.at("key");
            if (!key.empty()) {
                auto record = server_.GetKvRecord(collection, key, headers.count("x-trace-id") == 0 ? std::string() : headers.at("x-trace-id"));
                response = record.ok() ? json::BuildHttpResponse(200, "OK", "application/json", json::ToJson(record.value()).dump())
                                       : ErrorResponse(record.status());
            } else {
                const auto begin_key = params.count("begin_key") == 0 ? std::string() : params.at("begin_key");
                const auto end_key = params.count("end_key") == 0 ? std::string() : params.at("end_key");
                auto records = server_.RangeKvRecords(collection, begin_key, end_key, ParseLimit(params, 100));
                response = records.ok() ? json::BuildHttpResponse(200, "OK", "application/json", json::ToJson(records.value()).dump())
                                        : ErrorResponse(records.status());
            }
        } else if (method == "POST" || method == "PUT") {
            kvai::core::DocumentRecord record;
            if (const auto iterator = headers.find("x-trace-id"); iterator != headers.end()) {
                record.metadata["trace_id"] = iterator->second;
            }
            try {
                auto body_json = nlohmann::json::parse(body, nullptr, false);
                if (!body_json.is_discarded()) {
                    auto parsed = json::ParseDocumentUpsert(body_json);
                    if (parsed.ok()) {
                        record.collection = parsed.value().collection.empty() ? record.collection : parsed.value().collection;
                        record.key = parsed.value().key.empty() ? record.key : parsed.value().key;
                        record.title = parsed.value().title;
                        record.body = parsed.value().body;
                        record.metadata.insert(parsed.value().metadata.begin(), parsed.value().metadata.end());
                    }
                }
            } catch (...) {
                // Malformed JSON — let validation return a 400 if key is missing
            }
            auto status = server_.PutKvRecord(record, headers.count("x-trace-id") == 0 ? std::string() : headers.at("x-trace-id"));
            if (!status.ok()) {
                response = ErrorResponse(status);
            } else {
                nlohmann::json ok_json;
                ok_json["message"] = "kv record stored";
                response = json::BuildHttpResponse(200, "OK", "application/json", ok_json.dump());
            }
        } else if (method == "DELETE") {
            const auto collection = params.count("collection") == 0 ? std::string() : params.at("collection");
            const auto key = params.count("key") == 0 ? std::string() : params.at("key");
            auto status = server_.DeleteKvRecord(collection, key, headers.count("x-trace-id") == 0 ? std::string() : headers.at("x-trace-id"));
            if (!status.ok()) {
                response = ErrorResponse(status);
            } else {
                nlohmann::json ok_json;
                ok_json["message"] = "kv record deleted";
                response = json::BuildHttpResponse(200, "OK", "application/json", ok_json.dump());
            }
        } else {
            nlohmann::json error_json;
            error_json["message"] = "unsupported method for /v1/kv";
            response = json::BuildHttpResponse(405, "Method Not Allowed", "application/json", error_json.dump());
        }
    } else if (method == "POST" && path == "/v1/documents") {
        kvai::core::DocumentRecord record;
        if (const auto iterator = headers.find("x-trace-id"); iterator != headers.end()) {
            record.metadata["trace_id"] = iterator->second;
        }
        try {
            auto body_json = nlohmann::json::parse(body, nullptr, false);
            if (!body_json.is_discarded()) {
                auto parsed = json::ParseDocumentUpsert(body_json);
                if (parsed.ok()) {
                    record.collection = parsed.value().collection.empty() ? record.collection : parsed.value().collection;
                    record.key = parsed.value().key.empty() ? record.key : parsed.value().key;
                    record.title = parsed.value().title;
                    record.body = parsed.value().body;
                    record.metadata.insert(parsed.value().metadata.begin(), parsed.value().metadata.end());
                }
            }
        } catch (...) {
            // Malformed JSON — proceed with defaults
        }

        auto status = server_.UpsertDocument(record, headers.count("x-trace-id") == 0 ? std::string() : headers.at("x-trace-id"));
        if (!status.ok()) {
            nlohmann::json error_json;
            error_json["message"] = status.ToString();
            response = json::BuildHttpResponse(503, "Service Unavailable", "application/json", error_json.dump());
        } else {
            nlohmann::json ok_json;
            ok_json["message"] = "document upserted";
            response = json::BuildHttpResponse(200, "OK", "application/json", ok_json.dump());
        }
    } else if (method == "DELETE" && path == "/v1/documents") {
        const auto params = ParseQuery(query_string);
        const auto collection = params.count("collection") == 0 ? std::string() : params.at("collection");
        const auto key = params.count("key") == 0 ? std::string() : params.at("key");
        auto status = server_.DeleteDocument(collection, key, headers.count("x-trace-id") == 0 ? std::string() : headers.at("x-trace-id"));
        if (!status.ok()) {
            nlohmann::json error_json;
            error_json["message"] = status.ToString();
            response = json::BuildHttpResponse(503, "Service Unavailable", "application/json", error_json.dump());
        } else {
            nlohmann::json ok_json;
            ok_json["message"] = "document deleted";
            response = json::BuildHttpResponse(200, "OK", "application/json", ok_json.dump());
        }
    } else {
        nlohmann::json error_json;
        error_json["error"] = "route not found";
        response = json::BuildHttpResponse(404, "Not Found", "application/json", error_json.dump());
    }

    (void)::send(client_fd, response.data(), response.size(), 0);
    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
}

void HttpGatewayRuntime::WakeWaiters() {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    wait_condition_.notify_all();
}

}  // namespace kvai::gateway
