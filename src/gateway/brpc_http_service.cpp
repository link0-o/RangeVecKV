#include "gateway/brpc_http_service.h"

#include <algorithm>
#include <cctype>

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
        std::string key = it->first;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        headers[key] = it->second;
    }
    return headers;
}

std::map<std::string, std::string> QueryParamsFromController(brpc::Controller* cntl) {
    std::map<std::string, std::string> params;
    const auto& uri = cntl->http_request().uri();
    for (auto it = uri.QueryBegin(); it != uri.QueryEnd(); ++it) {
        params[it->first] = it->second;
    }
    return params;
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

void ReplyStatus(brpc::Controller* cntl, const kvai::infra::Status& status) {
    nlohmann::json error_json;
    error_json["message"] = status.ToString();
    ReplyJson(cntl, HttpStatusFor(status), error_json.dump());
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
        if (const auto it = params.find("image_path"); it != params.end()) {
            query.image_reference = it->second;
        } else if (const auto it = params.find("image_reference"); it != params.end()) {
            query.image_reference = it->second;
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

void BrpcHttpServiceImpl::Kv(google::protobuf::RpcController* controller,
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

    const auto params = QueryParamsFromController(cntl);
    const auto method = cntl->http_request().method();
    const auto trace_id = headers.count("x-trace-id") == 0 ? std::string() : headers["x-trace-id"];

    if (method == brpc::HTTP_METHOD_GET) {
        const auto collection = params.count("collection") == 0 ? std::string() : params.at("collection");
        const auto key = params.count("key") == 0 ? std::string() : params.at("key");
        if (!key.empty()) {
            auto record = server_.GetKvRecord(collection, key, trace_id);
            if (!record.ok()) {
                ReplyStatus(cntl, record.status());
                return;
            }
            ReplyJson(cntl, 200, json::ToJson(record.value()).dump());
            return;
        }

        const auto begin_key = params.count("begin_key") == 0 ? std::string() : params.at("begin_key");
        const auto end_key = params.count("end_key") == 0 ? std::string() : params.at("end_key");
        auto records = server_.RangeKvRecords(collection, begin_key, end_key, ParseLimit(params, 100));
        if (!records.ok()) {
            ReplyStatus(cntl, records.status());
            return;
        }
        ReplyJson(cntl, 200, json::ToJson(records.value()).dump());
        return;
    }

    if (method == brpc::HTTP_METHOD_POST || method == brpc::HTTP_METHOD_PUT) {
        kvai::core::DocumentRecord record;
        if (const auto iterator = headers.find("x-trace-id"); iterator != headers.end()) {
            record.metadata["trace_id"] = iterator->second;
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
            // Malformed JSON — let validation return a 400 if key is missing.
        }

        auto status = server_.PutKvRecord(record, trace_id);
        if (!status.ok()) {
            ReplyStatus(cntl, status);
            return;
        }
        nlohmann::json ok_json;
        ok_json["message"] = "kv record stored";
        ReplyJson(cntl, 200, ok_json.dump());
        return;
    }

    if (method == brpc::HTTP_METHOD_DELETE) {
        const auto collection = params.count("collection") == 0 ? std::string() : params.at("collection");
        const auto key = params.count("key") == 0 ? std::string() : params.at("key");
        auto status = server_.DeleteKvRecord(collection, key, trace_id);
        if (!status.ok()) {
            ReplyStatus(cntl, status);
            return;
        }
        nlohmann::json ok_json;
        ok_json["message"] = "kv record deleted";
        ReplyJson(cntl, 200, ok_json.dump());
        return;
    }

    nlohmann::json error_json;
    error_json["message"] = "unsupported method for /v1/kv";
    ReplyJson(cntl, 405, error_json.dump());
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
