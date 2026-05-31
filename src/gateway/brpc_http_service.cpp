#include "gateway/brpc_http_service.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>

#include <nlohmann/json.hpp>

#include "gateway/json_helpers.h"
#include "infra/logging.h"

namespace kvai::gateway {

namespace {

std::map<std::string, std::string> HeadersFromController(brpc::Controller* cntl) {
    std::map<std::string, std::string> headers;
    const auto& http_req = cntl->http_request();
    // Iterate over HTTP headers
    for (auto it = http_req.HeaderBegin(); it != http_req.HeaderEnd(); ++it) {
        std::string key = it->first.as_string();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        headers[key] = it->second;
    }
    return headers;
}

std::map<std::string, std::string> QueryParamsFromController(brpc::Controller* cntl) {
    std::map<std::string, std::string> params;
    const auto& uri = cntl->http_request().uri();
    // Parse query string from URI
    const auto& query = uri.query();
    for (auto it = query.begin(); it != query.end(); ++it) {
        params[it->first] = it->second;
    }
    return params;
}

bool IsPublicRoute(const std::string& path) {
    return path == "/healthz" || path == "/openapi" || path == "/openapi.yaml";
}

void ReplyJson(brpc::Controller* cntl, int status_code, const std::string& body) {
    cntl->http_response().set_content_type("application/json");
    cntl->http_response().set_status_code(status_code);
    cntl->response_attachment().append(body);
}

void ReplyText(brpc::Controller* cntl, int status_code, const std::string& content_type, const std::string& body) {
    cntl->http_response().set_content_type(content_type);
    cntl->http_response().set_status_code(status_code);
    cntl->response_attachment().append(body);
}

}  // namespace

BrpcHttpServiceImpl::BrpcHttpServiceImpl(InProcessGatewayServer& server, const ApiKeyAuthenticator& authenticator)
    : server_(server), authenticator_(authenticator) {}

void BrpcHttpServiceImpl::Search(google::protobuf::RpcController* controller,
                                  const kvai::v1::HttpRequest* /*request*/,
                                  kvai::v1::HttpResponse* /*response*/,
                                  google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);

    // Authentication
    auto headers = HeadersFromController(cntl);
    auto auth_status = authenticator_.Authenticate(headers);
    if (!auth_status.ok()) {
        nlohmann::json error_json;
        error_json["message"] = auth_status.ToString();
        cntl->http_response().set_status_code(401);
        ReplyJson(cntl, 401, error_json.dump());
        return;
    }

    SemanticSearchQuery query;
    if (const auto it = headers.find("x-trace-id"); it != headers.end()) {
        query.trace_id = it->second;
    }

    const auto method = cntl->http_request().method();
    if (method == brpc::HTTP_METHOD_GET) {
        auto params = QueryParamsFromController(cntl);
        if (const auto it = params.find("q"); it != params.end()) {
            query.query = it->second;
        }
        if (const auto it = params.find("collection"); it != params.end()) {
            query.collection = it->second;
        }
        if (const auto it = params.find("top_k"); it != params.end() && !it->second.empty()) {
            query.top_k = static_cast<std::size_t>(std::stoul(it->second));
        }
        for (const auto& [key, value] : params) {
            if (key.rfind("filter.", 0) == 0) {
                query.filters.push_back(SearchFilter{key.substr(7), value});
            }
        }
    } else if (method == brpc::HTTP_METHOD_POST) {
        auto body = cntl->request_attachment().to_string();
        try {
            auto body_json = nlohmann::json::parse(body, nullptr, false);
            if (!body_json.is_discarded()) {
                auto parsed = json::ParseSearchQuery(body_json, true);
                if (parsed.ok()) {
                    query.query = parsed.value().query.empty() ? query.query : parsed.value().query;
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
        ReplyJson(cntl, 503, error_json.dump());
    } else {
        ReplyJson(cntl, 200, json::ToJson(result.value()).dump());
    }
}

void BrpcHttpServiceImpl::Router(google::protobuf::RpcController* controller,
                                  const kvai::v1::HttpRequest* /*request*/,
                                  kvai::v1::HttpResponse* /*response*/,
                                  google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);

    auto headers = HeadersFromController(cntl);
    auto auth_status = authenticator_.Authenticate(headers);
    if (!auth_status.ok()) {
        nlohmann::json error_json;
        error_json["message"] = auth_status.ToString();
        ReplyJson(cntl, 401, error_json.dump());
        return;
    }

    auto params = QueryParamsFromController(cntl);
    const auto collection = params.count("collection") == 0 ? std::string() : params["collection"];
    const auto key = params.count("key") == 0 ? std::string() : params["key"];

    ReplyJson(cntl, 200, json::ToJson(server_.DescribeRoute(collection, key)).dump());
}

void BrpcHttpServiceImpl::UpsertDocument(google::protobuf::RpcController* controller,
                                          const kvai::v1::HttpRequest* /*request*/,
                                          kvai::v1::HttpResponse* /*response*/,
                                          google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);

    auto headers = HeadersFromController(cntl);
    auto auth_status = authenticator_.Authenticate(headers);
    if (!auth_status.ok()) {
        nlohmann::json error_json;
        error_json["message"] = auth_status.ToString();
        ReplyJson(cntl, 401, error_json.dump());
        return;
    }

    kvai::core::DocumentRecord record;
    if (const auto it = headers.find("x-trace-id"); it != headers.end()) {
        record.metadata["trace_id"] = it->second;
    }

    auto body = cntl->request_attachment().to_string();
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

    auto status = server_.UpsertDocument(record, headers.count("x-trace-id") == 0 ? std::string() : headers["x-trace-id"]);
    if (!status.ok()) {
        nlohmann::json error_json;
        error_json["message"] = status.ToString();
        ReplyJson(cntl, 503, error_json.dump());
    } else {
        nlohmann::json ok_json;
        ok_json["message"] = "document upserted";
        ReplyJson(cntl, 200, ok_json.dump());
    }
}

void BrpcHttpServiceImpl::DeleteDocument(google::protobuf::RpcController* controller,
                                          const kvai::v1::HttpRequest* /*request*/,
                                          kvai::v1::HttpResponse* /*response*/,
                                          google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);

    auto headers = HeadersFromController(cntl);
    auto auth_status = authenticator_.Authenticate(headers);
    if (!auth_status.ok()) {
        nlohmann::json error_json;
        error_json["message"] = auth_status.ToString();
        ReplyJson(cntl, 401, error_json.dump());
        return;
    }

    auto params = QueryParamsFromController(cntl);
    const auto collection = params.count("collection") == 0 ? std::string() : params["collection"];
    const auto key = params.count("key") == 0 ? std::string() : params["key"];

    auto status = server_.DeleteDocument(collection, key, headers.count("x-trace-id") == 0 ? std::string() : headers["x-trace-id"]);
    if (!status.ok()) {
        nlohmann::json error_json;
        error_json["message"] = status.ToString();
        ReplyJson(cntl, 503, error_json.dump());
    } else {
        nlohmann::json ok_json;
        ok_json["message"] = "document deleted";
        ReplyJson(cntl, 200, ok_json.dump());
    }
}

void BrpcHttpServiceImpl::Healthz(google::protobuf::RpcController* controller,
                                   const kvai::v1::HttpRequest* /*request*/,
                                   kvai::v1::HttpResponse* /*response*/,
                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);
    // /healthz is a public route — no auth required
    ReplyJson(cntl, 200, json::ToJson(server_.HealthCheck("")).dump());
}

void BrpcHttpServiceImpl::Metrics(google::protobuf::RpcController* controller,
                                   const kvai::v1::HttpRequest* /*request*/,
                                   kvai::v1::HttpResponse* /*response*/,
                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);

    auto headers = HeadersFromController(cntl);
    auto auth_status = authenticator_.Authenticate(headers);
    if (!auth_status.ok()) {
        nlohmann::json error_json;
        error_json["message"] = auth_status.ToString();
        ReplyJson(cntl, 401, error_json.dump());
        return;
    }

    ReplyText(cntl, 200, "text/plain; version=0.0.4", server_.Metrics().RenderPrometheus());
}

void BrpcHttpServiceImpl::OpenApi(google::protobuf::RpcController* controller,
                                   const kvai::v1::HttpRequest* /*request*/,
                                   kvai::v1::HttpResponse* /*response*/,
                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto* cntl = static_cast<brpc::Controller*>(controller);
    // /openapi.yaml is a public route
    ReplyText(cntl, 200, "application/yaml", server_.OpenApiSpec());
}

}  // namespace kvai::gateway
