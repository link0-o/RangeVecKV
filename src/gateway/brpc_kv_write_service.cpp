#include "gateway/brpc_kv_write_service.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace kvai::gateway {

namespace {

std::map<std::string, std::string> HeadersFromContext(const kvai::v1::RequestContext& context) {
    std::map<std::string, std::string> headers;
    if (!context.api_key().empty()) {
        headers["x-api-key"] = context.api_key();
    }
    if (!context.authorization().empty()) {
        headers["authorization"] = context.authorization();
    }
    if (!context.trace_id().empty()) {
        headers["x-trace-id"] = context.trace_id();
    }
    return headers;
}

kvai::core::DocumentRecord FromProto(const kvai::v1::DocumentRecord& proto) {
    kvai::core::DocumentRecord record;
    record.collection = proto.collection();
    record.key = proto.key();
    record.title = proto.title();
    record.body = proto.body();
    record.version = proto.version();
    record.updated_at_unix_ms = proto.updated_at_unix_ms();
    record.mutation_id = proto.mutation_id();
    for (const auto& [key, value] : proto.metadata()) {
        record.metadata[key] = value;
    }
    return record;
}

void Fail(google::protobuf::RpcController* controller, const kvai::infra::Status& status) {
    controller->SetFailed(status.ToString());
}

}  // namespace

BrpcKvWriteServiceImpl::BrpcKvWriteServiceImpl(InProcessGatewayServer& server, const ApiKeyAuthenticator& authenticator)
    : server_(server), authenticator_(authenticator) {}

void BrpcKvWriteServiceImpl::PutKv(google::protobuf::RpcController* controller,
                                   const kvai::v1::KvPutRequest* request,
                                   kvai::v1::KvWriteResponse* response,
                                   google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    const auto auth_status = authenticator_.Authenticate(HeadersFromContext(request->context()));
    if (!auth_status.ok()) {
        Fail(controller, auth_status);
        return;
    }

    auto status = server_.PutKvRecord(FromProto(request->record()), request->context().trace_id());
    if (!status.ok()) {
        Fail(controller, status);
        return;
    }
    response->set_trace_id(request->context().trace_id());
    response->set_message("kv record stored");
    response->set_record_count(1);
}

void BrpcKvWriteServiceImpl::BatchPutKv(google::protobuf::RpcController* controller,
                                        const kvai::v1::KvBatchPutRequest* request,
                                        kvai::v1::KvWriteResponse* response,
                                        google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    const auto auth_status = authenticator_.Authenticate(HeadersFromContext(request->context()));
    if (!auth_status.ok()) {
        Fail(controller, auth_status);
        return;
    }

    std::vector<kvai::core::DocumentRecord> records;
    records.reserve(static_cast<std::size_t>(request->records_size()));
    for (const auto& proto_record : request->records()) {
        records.push_back(FromProto(proto_record));
    }

    auto status = server_.PutKvRecords(std::move(records), request->context().trace_id());
    if (!status.ok()) {
        Fail(controller, status);
        return;
    }
    response->set_trace_id(request->context().trace_id());
    response->set_message("kv batch stored");
    response->set_record_count(static_cast<std::uint32_t>(request->records_size()));
}

void BrpcKvWriteServiceImpl::UpsertDocument(google::protobuf::RpcController* controller,
                                            const kvai::v1::DocumentUpsertRequest* request,
                                            kvai::v1::KvWriteResponse* response,
                                            google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);
    const auto auth_status = authenticator_.Authenticate(HeadersFromContext(request->context()));
    if (!auth_status.ok()) {
        Fail(controller, auth_status);
        return;
    }

    auto status = server_.UpsertDocument(FromProto(request->record()), request->context().trace_id());
    if (!status.ok()) {
        Fail(controller, status);
        return;
    }
    response->set_trace_id(request->context().trace_id());
    response->set_message("document upserted");
    response->set_record_count(1);
}

}  // namespace kvai::gateway
