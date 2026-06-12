#pragma once

#include <string>

#include "gateway/auth.h"
#include "gateway/server.h"
#include "search.pb.h"

namespace kvai::gateway {

class BrpcKvWriteServiceImpl : public kvai::v1::KvWriteService {
public:
    BrpcKvWriteServiceImpl(InProcessGatewayServer& server, const ApiKeyAuthenticator& authenticator);

    void PutKv(google::protobuf::RpcController* controller,
               const kvai::v1::KvPutRequest* request,
               kvai::v1::KvWriteResponse* response,
               google::protobuf::Closure* done) override;

    void BatchPutKv(google::protobuf::RpcController* controller,
                    const kvai::v1::KvBatchPutRequest* request,
                    kvai::v1::KvWriteResponse* response,
                    google::protobuf::Closure* done) override;

    void UpsertDocument(google::protobuf::RpcController* controller,
                        const kvai::v1::DocumentUpsertRequest* request,
                        kvai::v1::KvWriteResponse* response,
                        google::protobuf::Closure* done) override;

private:
    InProcessGatewayServer& server_;
    const ApiKeyAuthenticator& authenticator_;
};

}  // namespace kvai::gateway
