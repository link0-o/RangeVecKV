#include "ai/embedding_service.h"

#include <cmath>
#include <filesystem>
#include <functional>

#include "infra/logging.h"

#if defined(KVAI_HAVE_ONNXRUNTIME)
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#endif

namespace kvai::ai {

namespace {

std::vector<float> HashPayload(const std::string& payload, std::size_t dimensions) {
    std::vector<float> values(dimensions, 0.0F);
    const std::hash<std::string> hasher;
    for (std::size_t index = 0; index < dimensions; ++index) {
        const auto seed = payload + '#' + std::to_string(index);
        const auto hash_value = hasher(seed);
        values[index] = static_cast<float>((hash_value % 2000000ULL) / 1000000.0 - 1.0);
    }
    return values;
}

kvai::infra::Status Normalize(std::vector<float>& values) {
    double norm = 0.0;
    for (float value : values) {
        norm += static_cast<double>(value) * static_cast<double>(value);
    }
    norm = std::sqrt(norm);
    if (norm == 0.0) {
        return kvai::infra::Status::Internal("failed to normalize embedding");
    }
    for (float& value : values) {
        value = static_cast<float>(value / norm);
    }
    return kvai::infra::Status::Ok();
}

#if defined(KVAI_HAVE_ONNXRUNTIME)
class OnnxRuntimeEmbeddingService final : public EmbeddingService {
public:
    explicit OnnxRuntimeEmbeddingService(const kvai::infra::ServerConfig& config)
        : env_(ORT_LOGGING_LEVEL_WARNING, "rangeveckv"),
          session_options_(),
          dimensions_(config.embedding_dimensions) {
        session_options_.SetIntraOpNumThreads(1);
        session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                session_ = std::make_unique<Ort::Session>(env_, config.model_path.c_str(), session_options_);
    }

    kvai::infra::StatusOr<Embedding> EmbedText(const std::string& text) const override {
        if (text.empty()) {
            return kvai::infra::Status::InvalidArgument("text query cannot be empty");
        }
        return EmbedPayload(text, "onnxruntime-text");
    }

    kvai::infra::StatusOr<Embedding> EmbedImage(const std::string& image_reference) const override {
        if (image_reference.empty()) {
            return kvai::infra::Status::InvalidArgument("image reference cannot be empty");
        }
        return EmbedPayload(image_reference, "onnxruntime-image");
    }

private:
    kvai::infra::StatusOr<Embedding> EmbedPayload(const std::string& payload, const std::string& backend) const {
        if (dimensions_ == 0) {
            return kvai::infra::Status::InvalidArgument("embedding dimensions must be positive");
        }

        std::vector<float> input = HashPayload(payload, dimensions_);
        auto status = Normalize(input);
        if (!status.ok()) {
            return status;
        }

        std::vector<int64_t> shape{1, static_cast<int64_t>(dimensions_)};
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, input.data(), input.size(), shape.data(), shape.size());

        Ort::AllocatorWithDefaultOptions allocator;
        auto input_name = session_->GetInputNameAllocated(0, allocator);
        auto output_name = session_->GetOutputNameAllocated(0, allocator);
        const char* input_names[] = {input_name.get()};
        const char* output_names[] = {output_name.get()};

        try {
            auto outputs = session_->Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);
            if (outputs.empty() || !outputs.front().IsTensor()) {
                return kvai::infra::Status::Internal("onnxruntime produced no tensor output");
            }

            const auto& output_tensor = outputs.front();
            const auto shape_info = output_tensor.GetTensorTypeAndShapeInfo();
            const auto output_count = shape_info.GetElementCount();
            const float* output_data = output_tensor.GetTensorData<float>();

            Embedding embedding;
            embedding.backend = backend;
            embedding.values.assign(output_data, output_data + std::min<std::size_t>(dimensions_, output_count));
            if (embedding.values.size() < dimensions_) {
                embedding.values.resize(dimensions_, 0.0F);
            }
            status = Normalize(embedding.values);
            if (!status.ok()) {
                return status;
            }
            return embedding;
        } catch (const Ort::Exception& error) {
            return kvai::infra::Status::Unavailable(std::string("onnxruntime inference failed: ") + error.what());
        }
    }

    Ort::Env env_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    std::size_t dimensions_;
};
#endif

}  // namespace

DeterministicEmbeddingService::DeterministicEmbeddingService(std::size_t dimensions) : dimensions_(dimensions) {}

kvai::infra::StatusOr<Embedding> DeterministicEmbeddingService::EmbedText(const std::string& text) const {
    if (text.empty()) {
        return kvai::infra::Status::InvalidArgument("text query cannot be empty");
    }
    return EmbedPayload(text, "deterministic-text");
}

kvai::infra::StatusOr<Embedding> DeterministicEmbeddingService::EmbedImage(const std::string& image_reference) const {
    if (image_reference.empty()) {
        return kvai::infra::Status::InvalidArgument("image reference cannot be empty");
    }
    return EmbedPayload(image_reference, "deterministic-image");
}

kvai::infra::StatusOr<Embedding> DeterministicEmbeddingService::EmbedPayload(const std::string& payload, const std::string& backend) const {
    if (dimensions_ == 0) {
        return kvai::infra::Status::InvalidArgument("embedding dimensions must be positive");
    }

    Embedding embedding;
    embedding.values = HashPayload(payload, dimensions_);
    embedding.backend = backend;
    auto status = Normalize(embedding.values);
    if (!status.ok()) {
        return status;
    }
    return embedding;
}

kvai::infra::StatusOr<std::unique_ptr<EmbeddingService>> CreateEmbeddingService(const kvai::infra::ServerConfig& config) {
#if defined(KVAI_HAVE_ONNXRUNTIME)
    if (config.ai_backend == "onnxruntime" || config.ai_backend == "auto") {
        if (!config.model_path.empty()) {
            namespace fs = std::filesystem;
            if (fs::exists(config.model_path)) {
                try {
                    kvai::infra::log::Info("ai", "initializing ONNX Runtime embedding service", {{"model_path", config.model_path}});
                    return std::make_unique<OnnxRuntimeEmbeddingService>(config);
                } catch (const Ort::Exception& error) {
                    kvai::infra::log::Warn("ai", "ONNX Runtime initialization failed, falling back to deterministic",
                                           {{"error", error.what()}});
                }
            } else {
                kvai::infra::log::Warn("ai", "model file not found, falling back to deterministic",
                                       {{"model_path", config.model_path}});
            }
        }
        if (config.ai_backend == "onnxruntime") {
            // Explicitly requested but unavailable
            if (config.model_path.empty()) {
                return kvai::infra::Status::InvalidArgument("ai.model_path must be configured for onnxruntime backend");
            }
            return kvai::infra::Status::Unavailable("onnxruntime backend requested but model file not found: " + config.model_path);
        }
        // auto mode: fall through to deterministic
    }
#else
    if (config.ai_backend == "onnxruntime") {
        return kvai::infra::Status::NotSupported("onnxruntime backend requested but library is not available in this build");
    }
#endif

    if (config.ai_backend != "deterministic" && config.ai_backend != "auto") {
        return kvai::infra::Status::InvalidArgument("unsupported ai backend: " + config.ai_backend);
    }
    if (config.ai_backend == "auto") {
        kvai::infra::log::Info("ai", "using deterministic embedding fallback (no ONNX Runtime available)");
    }
    return std::unique_ptr<EmbeddingService>(std::make_unique<DeterministicEmbeddingService>(config.embedding_dimensions));
}

}  // namespace kvai::ai