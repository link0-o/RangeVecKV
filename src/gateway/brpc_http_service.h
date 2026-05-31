#pragma once

#include <string>

#include "search.pb.h"
#include "gateway/auth.h"
#include "gateway/server.h"

namespace kvai::gateway {

/// Implements GatewayService from search.proto.
/// Handles all HTTP REST endpoints via BRPC.
class BrpcHttpServiceImpl : public kvai::v1::GatewayService {
public:
    BrpcHttpServiceImpl(InProcessGatewayServer& server, const ApiKeyAuthenticator& authenticator);

    void Search(google::protobuf::RpcController* controller,
                const kvai::v1::HttpRequest* request,
                kvai::v1::HttpResponse* response,
                google::protobuf::Closure* done) override;

    void Router(google::protobuf::RpcController* controller,
                const kvai::v1::HttpRequest* request,
                kvai::v1::HttpResponse* response,
                google::protobuf::Closure* done) override;

    void UpsertDocument(google::protobuf::RpcController* controller,
                        const kvai::v1::HttpRequest* request,
                        kvai::v1::HttpResponse* response,
                        google::protobuf::Closure* done) override;

    void DeleteDocument(google::protobuf::RpcController* controller,
                        const kvai::v1::HttpRequest* request,
                        kvai::v1::HttpResponse* response,
                        google::protobuf::Closure* done) override;

    void Healthz(google::protobuf::RpcController* controller,
                 const kvai::v1::HttpRequest* request,
                 kvai::v1::HttpResponse* response,
                 google::protobuf::Closure* done) override;

    void Metrics(google::protobuf::RpcController* controller,
                 const kvai::v1::HttpRequest* request,
                 kvai::v1::HttpResponse* response,
                 google::protobuf::Closure* done) override;

    void OpenApi(google::protobuf::RpcController* controller,
                 const kvai::v1::HttpRequest* request,
                 kvai::v1::HttpResponse* response,
                 google::protobuf::Closure* done) override;

private:
    InProcessGatewayServer& server_;
    const ApiKeyAuthenticator& authenticator_;
};

}  // namespace kvai::gateway
