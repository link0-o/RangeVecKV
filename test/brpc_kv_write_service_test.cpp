#include <brpc/controller.h>

#include <filesystem>
#include <iostream>

#include "gateway/brpc_kv_write_service.h"
#include "gateway/server.h"

namespace {

bool Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

void FillContext(kvai::v1::RequestContext* context, const std::string& api_key) {
    context->set_trace_id("brpc-kv-test-trace");
    context->set_api_key(api_key);
}

void FillRecord(kvai::v1::DocumentRecord* record,
                const std::string& collection,
                const std::string& key,
                const std::string& body,
                std::uint64_t version,
                const std::string& mutation_id) {
    record->set_collection(collection);
    record->set_key(key);
    record->set_body(body);
    record->set_version(version);
    record->set_updated_at_unix_ms(static_cast<std::int64_t>(version));
    record->set_mutation_id(mutation_id);
    (*record->mutable_metadata())["kind"] = "protobuf-test";
}

}  // namespace

int main() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "rangeveckv-brpc-kv-write-test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    kvai::infra::ServerConfig config;
    config.require_api_key = true;
    config.api_key = "secret-token";
    config.enable_demo_data = false;
    config.wal_path = (temp_dir / "gateway.wal").string();
    config.snapshot_path = (temp_dir / "gateway.snapshot").string();
    config.db_path = (temp_dir / "rocksdb").string();
    config.index_path = (temp_dir / "gateway.index").string();
    config.vector_index_outbox_path = (temp_dir / "gateway.outbox").string();
    config.migration_task_wal_path = (temp_dir / "migration_tasks.json").string();

    kvai::gateway::InProcessGatewayServer server(config);
    if (!Expect(server.Start().ok(), "server start failed")) {
        return 1;
    }

    kvai::gateway::ApiKeyAuthenticator authenticator(config.api_key, config.require_api_key);
    kvai::gateway::BrpcKvWriteServiceImpl service(server, authenticator);

    {
        brpc::Controller controller;
        kvai::v1::KvPutRequest request;
        kvai::v1::KvWriteResponse response;
        FillContext(request.mutable_context(), "wrong-token");
        FillRecord(request.mutable_record(), "kv", "auth-fail", "blocked", 1, "m-auth-fail");
        service.PutKv(&controller, &request, &response, nullptr);
        if (!Expect(controller.Failed(), "invalid protobuf api key should fail")) {
            return 1;
        }
    }

    {
        brpc::Controller controller;
        kvai::v1::KvPutRequest request;
        kvai::v1::KvWriteResponse response;
        FillContext(request.mutable_context(), "secret-token");
        FillRecord(request.mutable_record(), "kv", "pb:1", "protobuf value", 100, "m-pb-1");
        service.PutKv(&controller, &request, &response, nullptr);
        if (!Expect(!controller.Failed(), "protobuf PutKv failed: " + controller.ErrorText())) {
            return 1;
        }
        if (!Expect(response.record_count() == 1, "PutKv response record_count mismatch")) {
            return 1;
        }

        auto stored = server.GetKvRecord("kv", "pb:1", "");
        if (!Expect(stored.ok() && stored.value().body == "protobuf value", "protobuf PutKv not visible through JSON-compatible read path")) {
            return 1;
        }
    }

    {
        brpc::Controller controller;
        kvai::v1::KvPutRequest request;
        kvai::v1::KvWriteResponse response;
        FillContext(request.mutable_context(), "secret-token");
        FillRecord(request.mutable_record(), "kv", "pb:1", "stale value", 99, "m-stale");
        service.PutKv(&controller, &request, &response, nullptr);
        if (!Expect(controller.Failed(), "stale protobuf PutKv should fail")) {
            return 1;
        }
    }

    {
        brpc::Controller controller;
        kvai::v1::KvPutRequest request;
        kvai::v1::KvWriteResponse response;
        FillContext(request.mutable_context(), "secret-token");
        FillRecord(request.mutable_record(), "kv", "pb:1", "protobuf value", 100, "m-pb-1");
        service.PutKv(&controller, &request, &response, nullptr);
        if (!Expect(!controller.Failed(), "duplicate mutation should be idempotent")) {
            return 1;
        }
    }

    {
        brpc::Controller controller;
        kvai::v1::KvBatchPutRequest request;
        kvai::v1::KvWriteResponse response;
        FillContext(request.mutable_context(), "secret-token");
        FillRecord(request.add_records(), "kv", "pb:batch:1", "batch value 1", 200, "m-batch-1");
        FillRecord(request.add_records(), "kv", "pb:batch:2", "batch value 2", 201, "m-batch-2");
        service.BatchPutKv(&controller, &request, &response, nullptr);
        if (!Expect(!controller.Failed(), "protobuf BatchPutKv failed: " + controller.ErrorText())) {
            return 1;
        }
        if (!Expect(response.record_count() == 2, "BatchPutKv response record_count mismatch")) {
            return 1;
        }
        auto stored = server.GetKvRecord("kv", "pb:batch:2", "");
        if (!Expect(stored.ok() && stored.value().body == "batch value 2", "protobuf batch record missing")) {
            return 1;
        }
    }

    if (!Expect(server.Stop().ok(), "server stop failed")) {
        return 1;
    }

    std::filesystem::remove_all(temp_dir);
    return 0;
}
