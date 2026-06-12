#include <brpc/channel.h>
#include <brpc/controller.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "search.pb.h"

namespace {

struct Options {
    std::string server = "127.0.0.1:8080";
    std::string api_key;
    std::string mode = "kv";
    std::string output;
    int threads = 16;
    int duration_seconds = 30;
    int warmup_seconds = 5;
    int timeout_ms = 10000;
    int batch_size = 100;
};

struct Result {
    std::uint64_t successes = 0;
    std::uint64_t record_successes = 0;
    double elapsed_seconds = 0.0;
    std::vector<double> latencies_ms;
    std::map<std::string, std::uint64_t> errors;
};

std::int64_t UnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double Percentile(const std::vector<double>& values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(
        std::max<double>(0.0, std::min<double>(static_cast<double>(values.size() - 1),
                                               (percentile / 100.0) * static_cast<double>(values.size() - 1))));
    return values[index];
}

std::string JsonEscape(const std::string& value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        default:
            output.push_back(ch);
            break;
        }
    }
    return output;
}

void FillContext(kvai::v1::RequestContext* context, const Options& options, std::uint64_t sequence) {
    context->set_trace_id("brpc-bench-" + std::to_string(sequence));
    context->set_api_key(options.api_key);
}

void FillRecord(kvai::v1::DocumentRecord* record, const std::string& collection, const std::string& key, const std::string& body) {
    const auto now = UnixMillis();
    record->set_collection(collection);
    record->set_key(key);
    record->set_body(body);
    record->set_version(static_cast<std::uint64_t>(now));
    record->set_updated_at_unix_ms(now);
    record->set_mutation_id("brpc-bench-" + key);
    (*record->mutable_metadata())["kind"] = "benchmark";
}

bool RequestOnce(kvai::v1::KvWriteService_Stub& stub,
                 const Options& options,
                 std::uint64_t sequence,
                 std::string* error) {
    brpc::Controller controller;
    controller.set_timeout_ms(options.timeout_ms);
    kvai::v1::KvWriteResponse response;

    if (options.mode == "kv") {
        kvai::v1::KvPutRequest request;
        FillContext(request.mutable_context(), options, sequence);
        const auto key = "brpc-kv-" + std::to_string(sequence);
        FillRecord(request.mutable_record(), "bench_kv", key, "pure kv protobuf benchmark payload " + std::to_string(sequence));
        stub.PutKv(&controller, &request, &response, nullptr);
    } else if (options.mode == "kv_batch") {
        kvai::v1::KvBatchPutRequest request;
        FillContext(request.mutable_context(), options, sequence);
        const auto base = sequence * static_cast<std::uint64_t>(options.batch_size);
        for (int offset = 0; offset < options.batch_size; ++offset) {
            const auto item = base + static_cast<std::uint64_t>(offset);
            const auto key = "brpc-kv-batch-" + std::to_string(item);
            FillRecord(request.add_records(), "bench_kv", key, "pure kv protobuf batch benchmark payload " + std::to_string(item));
        }
        stub.BatchPutKv(&controller, &request, &response, nullptr);
    } else {
        kvai::v1::DocumentUpsertRequest request;
        FillContext(request.mutable_context(), options, sequence);
        const auto key = "brpc-doc-" + std::to_string(sequence);
        auto* record = request.mutable_record();
        FillRecord(record,
                   "bench_documents",
                   key,
                   "RangeVecKV protobuf benchmark document with semantic vector indexing " + std::to_string(sequence));
        record->set_title("Protobuf Vector Benchmark " + std::to_string(sequence));
        stub.UpsertDocument(&controller, &request, &response, nullptr);
    }

    if (controller.Failed()) {
        *error = controller.ErrorText().empty() ? "rpc_failed" : controller.ErrorText();
        return false;
    }
    return true;
}

Result Run(const Options& options, int duration_seconds) {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> successes{0};
    std::mutex latencies_mutex;
    std::mutex errors_mutex;
    std::vector<double> latencies;
    std::map<std::string, std::uint64_t> errors;

    const auto start = std::chrono::steady_clock::now();
    const auto stop = start + std::chrono::seconds(duration_seconds);

    auto worker = [&]() {
        brpc::Channel channel;
        brpc::ChannelOptions channel_options;
        channel_options.protocol = "baidu_std";
        channel_options.timeout_ms = options.timeout_ms;
        channel_options.max_retry = 0;
        if (channel.Init(options.server.c_str(), "", &channel_options) != 0) {
            std::lock_guard<std::mutex> lock(errors_mutex);
            ++errors["channel_init_failed"];
            return;
        }
        kvai::v1::KvWriteService_Stub stub(&channel);
        std::vector<double> local_latencies;
        std::map<std::string, std::uint64_t> local_errors;
        std::uint64_t local_successes = 0;

        while (std::chrono::steady_clock::now() < stop) {
            const auto item = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto before = std::chrono::steady_clock::now();
            std::string error;
            if (RequestOnce(stub, options, item, &error)) {
                ++local_successes;
                const auto after = std::chrono::steady_clock::now();
                local_latencies.push_back(std::chrono::duration<double, std::milli>(after - before).count());
            } else {
                ++local_errors[error];
            }
        }

        successes.fetch_add(local_successes, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(latencies_mutex);
            latencies.insert(latencies.end(), local_latencies.begin(), local_latencies.end());
        }
        {
            std::lock_guard<std::mutex> lock(errors_mutex);
            for (const auto& [key, value] : local_errors) {
                errors[key] += value;
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(options.threads));
    for (int index = 0; index < options.threads; ++index) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
        thread.join();
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::sort(latencies.begin(), latencies.end());

    Result result;
    result.successes = successes.load(std::memory_order_relaxed);
    result.record_successes = result.successes * static_cast<std::uint64_t>(options.mode == "kv_batch" ? options.batch_size : 1);
    result.elapsed_seconds = elapsed;
    result.latencies_ms = std::move(latencies);
    result.errors = std::move(errors);
    return result;
}

void PrintResult(const Options& options, const Result& result, std::ostream& output) {
    const auto request_qps = result.elapsed_seconds > 0.0 ? static_cast<double>(result.successes) / result.elapsed_seconds : 0.0;
    const auto records_per_second = result.elapsed_seconds > 0.0 ? static_cast<double>(result.record_successes) / result.elapsed_seconds : 0.0;
    const auto average = result.latencies_ms.empty()
                             ? 0.0
                             : std::accumulate(result.latencies_ms.begin(), result.latencies_ms.end(), 0.0) /
                                   static_cast<double>(result.latencies_ms.size());

    output << "{\n"
           << "  \"wire\": \"protobuf_brpc\",\n"
           << "  \"mode\": \"" << JsonEscape(options.mode) << "\",\n"
           << "  \"server\": \"" << JsonEscape(options.server) << "\",\n"
           << "  \"threads\": " << options.threads << ",\n"
           << "  \"duration_seconds\": " << result.elapsed_seconds << ",\n"
           << "  \"successes\": " << result.successes << ",\n"
           << "  \"records_per_success\": " << (options.mode == "kv_batch" ? options.batch_size : 1) << ",\n"
           << "  \"record_successes\": " << result.record_successes << ",\n"
           << "  \"qps\": " << request_qps << ",\n"
           << "  \"records_per_second\": " << records_per_second << ",\n"
           << "  \"latency_ms\": {\n"
           << "    \"avg\": " << average << ",\n"
           << "    \"p50\": " << Percentile(result.latencies_ms, 50) << ",\n"
           << "    \"p95\": " << Percentile(result.latencies_ms, 95) << ",\n"
           << "    \"p99\": " << Percentile(result.latencies_ms, 99) << ",\n"
           << "    \"max\": " << (result.latencies_ms.empty() ? 0.0 : result.latencies_ms.back()) << "\n"
           << "  },\n"
           << "  \"errors\": {";

    bool first = true;
    for (const auto& [key, value] : result.errors) {
        output << (first ? "" : ",") << "\n    \"" << JsonEscape(key) << "\": " << value;
        first = false;
    }
    output << (first ? "" : "\n  ") << "}\n"
           << "}\n";
}

Options ParseArgs(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto next = [&]() -> std::string {
            if (index + 1 >= argc) {
                std::cerr << "missing value for " << arg << std::endl;
                std::exit(2);
            }
            return argv[++index];
        };
        if (arg == "--server") {
            options.server = next();
        } else if (arg == "--api-key") {
            options.api_key = next();
        } else if (arg == "--mode") {
            options.mode = next();
        } else if (arg == "--threads") {
            options.threads = std::stoi(next());
        } else if (arg == "--duration-seconds") {
            options.duration_seconds = std::stoi(next());
        } else if (arg == "--warmup-seconds") {
            options.warmup_seconds = std::stoi(next());
        } else if (arg == "--timeout-ms") {
            options.timeout_ms = std::stoi(next());
        } else if (arg == "--batch-size") {
            options.batch_size = std::stoi(next());
        } else if (arg == "--output") {
            options.output = next();
        } else {
            std::cerr << "unknown argument: " << arg << std::endl;
            std::exit(2);
        }
    }
    if (options.mode != "kv" && options.mode != "kv_batch" && options.mode != "vector") {
        std::cerr << "--mode must be kv, kv_batch, or vector" << std::endl;
        std::exit(2);
    }
    options.threads = std::max(1, options.threads);
    options.duration_seconds = std::max(1, options.duration_seconds);
    options.warmup_seconds = std::max(0, options.warmup_seconds);
    options.batch_size = std::max(1, options.batch_size);
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = ParseArgs(argc, argv);
    if (options.warmup_seconds > 0) {
        (void)Run(options, options.warmup_seconds);
    }

    const auto result = Run(options, options.duration_seconds);
    PrintResult(options, result, std::cout);
    if (!options.output.empty()) {
        std::ofstream output(options.output);
        if (!output.is_open()) {
            std::cerr << "failed to open output file: " << options.output << std::endl;
            return 1;
        }
        PrintResult(options, result, output);
    }
    return result.errors.empty() ? 0 : 1;
}
