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

#include "core/document.h"

#include "infra/logging.h"

namespace kvai::gateway {

namespace {

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '\\':
        case '"':
            escaped.push_back('\\');
            escaped.push_back(ch);
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

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
    std::map<std::string, std::string> headers;         // "host" -> "example.com"
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

std::string ExtractJsonString(const std::string& body, const std::string& key) {
    const std::string field = std::string("\"") + key + '"';
    const auto key_pos = body.find(field);
    if (key_pos == std::string::npos) {
        return {};
    }
    const auto colon_pos = body.find(':', key_pos + field.size());
    const auto quote_pos = body.find('"', colon_pos + 1);
    if (colon_pos == std::string::npos || quote_pos == std::string::npos) {
        return {};
    }

    std::string value;
    bool escaping = false;
    for (std::size_t index = quote_pos + 1; index < body.size(); ++index) {
        const char ch = body[index];
        if (escaping) {
            value.push_back(ch == 'n' ? '\n' : ch);
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }

    return {};
}

std::size_t ExtractJsonSize(const std::string& body, const std::string& key, std::size_t default_value) {
    const std::string field = std::string("\"") + key + '"';
    const auto key_pos = body.find(field);
    if (key_pos == std::string::npos) {
        return default_value;
    }
    const auto colon_pos = body.find(':', key_pos + field.size());
    if (colon_pos == std::string::npos) {
        return default_value;
    }
    std::size_t begin = colon_pos + 1;
    while (begin < body.size() && std::isspace(static_cast<unsigned char>(body[begin])) != 0) {
        ++begin;
    }
    std::size_t end = begin;
    while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end])) != 0) {
        ++end;
    }
    return end > begin ? static_cast<std::size_t>(std::stoul(body.substr(begin, end - begin))) : default_value;
}

std::map<std::string, std::string> ExtractJsonStringMap(const std::string& body, const std::string& key) {
    std::map<std::string, std::string> values;
    const std::string field = std::string("\"") + key + '"';
    const auto key_pos = body.find(field);
    if (key_pos == std::string::npos) {
        return values;
    }
    const auto open_brace = body.find('{', key_pos + field.size());
    if (open_brace == std::string::npos) {
        return values;
    }

    int depth = 0;
    std::size_t close_brace = open_brace;
    for (; close_brace < body.size(); ++close_brace) {
        if (body[close_brace] == '{') {
            ++depth;
        } else if (body[close_brace] == '}') {
            --depth;
            if (depth == 0) {
                break;
            }
        }
    }
    if (close_brace <= open_brace) {
        return values;
    }

    const auto object_body = body.substr(open_brace + 1, close_brace - open_brace - 1);
    std::stringstream stream(object_body);
    std::string token;
    while (std::getline(stream, token, ',')) {
        const auto colon_pos = token.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }
        auto map_key = token.substr(0, colon_pos);
        auto map_value = token.substr(colon_pos + 1);
        map_key.erase(std::remove_if(map_key.begin(), map_key.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '"'; }), map_key.end());
        map_value.erase(std::remove_if(map_value.begin(), map_value.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '"'; }), map_value.end());
        if (!map_key.empty()) {
            values[std::move(map_key)] = std::move(map_value);
        }
    }
    return values;
}

std::string MakeJsonResponse(const HealthReport& report) {
    std::ostringstream output;
    output << "{\n"
           << "  \"trace_id\": \"" << JsonEscape(report.trace_id) << "\",\n"
           << "  \"status\": \"" << JsonEscape(report.status) << "\",\n"
           << "  \"version\": \"" << JsonEscape(report.version) << "\",\n"
           << "  \"warnings\": [";
    for (std::size_t index = 0; index < report.warnings.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        output << '"' << JsonEscape(report.warnings[index]) << '"';
    }
    output << "],\n  \"details\": {";
    std::size_t emitted = 0;
    for (const auto& [key, value] : report.details) {
        if (emitted++ > 0) {
            output << ", ";
        }
        output << '"' << JsonEscape(key) << "\": \"" << JsonEscape(value) << '"';
    }
    output << "}\n}\n";
    return output.str();
}

std::string MakeJsonResponse(const SemanticSearchResult& result) {
    std::ostringstream output;
    output << "{\n"
           << "  \"trace_id\": \"" << JsonEscape(result.trace_id) << "\",\n"
           << "  \"degraded\": " << (result.degraded ? "true" : "false") << ",\n"
           << "  \"message\": \"" << JsonEscape(result.message) << "\",\n"
           << "  \"hits\": [\n";
    for (std::size_t index = 0; index < result.hits.size(); ++index) {
        const auto& hit = result.hits[index];
        output << "    {\"key\": \"" << JsonEscape(hit.key) << "\", \"title\": \"" << JsonEscape(hit.title)
               << "\", \"snippet\": \"" << JsonEscape(hit.snippet) << "\", \"score\": " << hit.score << ", \"metadata\": {";
        std::size_t metadata_index = 0;
        for (const auto& [key, value] : hit.metadata) {
            if (metadata_index++ > 0) {
                output << ", ";
            }
            output << '"' << JsonEscape(key) << "\": \"" << JsonEscape(value) << '"';
        }
        output << "}}";
        if (index + 1 < result.hits.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

std::string BuildHttpResponse(int status_code, const std::string& status_text, const std::string& content_type, const std::string& body) {
    std::ostringstream output;
    output << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n"
           << body;
    return output.str();
}

std::string MakeRouteResponse(const kvai::infra::RouteDecision& route) {
    std::ostringstream output;
    output << "{\n"
           << "  \"has_primary\": " << (route.has_primary ? "true" : "false") << ",\n"
           << "  \"local_owner\": " << (route.local_owner ? "true" : "false") << ",\n"
           << "  \"primary\": {\"id\": \"" << JsonEscape(route.primary.id) << "\", \"host\": \"" << JsonEscape(route.primary.host)
           << "\", \"port\": " << route.primary.port << "},\n"
           << "  \"replicas\": [";
    for (std::size_t index = 0; index < route.replicas.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        const auto& replica = route.replicas[index];
        output << "{\"id\": \"" << JsonEscape(replica.id) << "\", \"host\": \"" << JsonEscape(replica.host) << "\", \"port\": " << replica.port << "}";
    }
    output << "]\n}\n";
    return output.str();
}

std::string MakeStatusResponse(const std::string& message) {
    return std::string("{\"message\": \"") + JsonEscape(message) + "\"}\n";
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
    kvai::infra::Logger::Instance().Log(kvai::infra::LogLevel::kInfo,
                                        "http-runtime",
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
            kvai::infra::Logger::Instance().Log(kvai::infra::LogLevel::kWarn,
                                                "http-runtime",
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
            response = BuildHttpResponse(401, "Unauthorized", "application/json", MakeStatusResponse(auth_status.ToString()));
            (void)::send(client_fd, response.data(), response.size(), 0);
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
            return;
        }
    }

    if (method == "GET" && path == "/healthz") {
        response = BuildHttpResponse(200, "OK", "application/json", MakeJsonResponse(server_.HealthCheck("")));
    } else if (method == "GET" && path == "/metrics") {
        response = BuildHttpResponse(200, "OK", "text/plain; version=0.0.4", server_.Metrics().RenderPrometheus());
    } else if (method == "GET" && (path == "/openapi" || path == "/openapi.yaml")) {
        response = BuildHttpResponse(200, "OK", "application/yaml", server_.OpenApiSpec());
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
            if (const auto iterator = params.find("collection"); iterator != params.end()) {
                query.collection = iterator->second;
            }
            if (const auto iterator = params.find("top_k"); iterator != params.end() && !iterator->second.empty()) {
                query.top_k = static_cast<std::size_t>(std::stoul(iterator->second));
            }
            query.filters = ExtractFilters(params);
        } else {
            query.query = ExtractJsonString(body, "query");
            query.collection = ExtractJsonString(body, "collection");
            query.top_k = ExtractJsonSize(body, "top_k", query.top_k);
            if (const auto iterator = ExtractJsonStringMap(body, "filters"); !iterator.empty()) {
                for (const auto& [key, value] : iterator) {
                    query.filters.push_back(SearchFilter{key, value});
                }
            }
        }

        auto result = server_.Search(query);
        if (!result.ok()) {
            response = BuildHttpResponse(503,
                                         "Service Unavailable",
                                         "application/json",
                                         std::string("{\"error\": \"") + JsonEscape(result.status().ToString()) + "\"}\n");
        } else {
            response = BuildHttpResponse(200, "OK", "application/json", MakeJsonResponse(result.value()));
        }
    } else if (method == "GET" && path == "/v1/router") {
        const auto params = ParseQuery(query_string);
        const auto collection = params.count("collection") == 0 ? std::string() : params.at("collection");
        const auto key = params.count("key") == 0 ? std::string() : params.at("key");
        response = BuildHttpResponse(200, "OK", "application/json", MakeRouteResponse(server_.DescribeRoute(collection, key)));
    } else if (method == "POST" && path == "/v1/documents") {
        kvai::core::DocumentRecord record;
        if (const auto iterator = headers.find("x-trace-id"); iterator != headers.end()) {
            record.metadata["trace_id"] = iterator->second;
        }
        record.collection = ExtractJsonString(body, "collection");
        record.key = ExtractJsonString(body, "key");
        record.title = ExtractJsonString(body, "title");
        record.body = ExtractJsonString(body, "body");
        const auto metadata = ExtractJsonStringMap(body, "metadata");
        record.metadata.insert(metadata.begin(), metadata.end());

        auto status = server_.UpsertDocument(record, headers.count("x-trace-id") == 0 ? std::string() : headers.at("x-trace-id"));
        if (!status.ok()) {
            response = BuildHttpResponse(503, "Service Unavailable", "application/json", MakeStatusResponse(status.ToString()));
        } else {
            response = BuildHttpResponse(200, "OK", "application/json", MakeStatusResponse("document upserted"));
        }
    } else if (method == "DELETE" && path == "/v1/documents") {
        const auto params = ParseQuery(query_string);
        const auto collection = params.count("collection") == 0 ? std::string() : params.at("collection");
        const auto key = params.count("key") == 0 ? std::string() : params.at("key");
        auto status = server_.DeleteDocument(collection, key, headers.count("x-trace-id") == 0 ? std::string() : headers.at("x-trace-id"));
        if (!status.ok()) {
            response = BuildHttpResponse(503, "Service Unavailable", "application/json", MakeStatusResponse(status.ToString()));
        } else {
            response = BuildHttpResponse(200, "OK", "application/json", MakeStatusResponse("document deleted"));
        }
    } else {
        response = BuildHttpResponse(404, "Not Found", "application/json", "{\"error\": \"route not found\"}\n");
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