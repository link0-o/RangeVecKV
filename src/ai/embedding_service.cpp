#include "ai/embedding_service.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "infra/logging.h"

#if defined(KVAI_HAVE_ONNXRUNTIME)
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>
#endif

namespace kvai::ai {

namespace {

std::vector<std::string> TokenizeText(const std::string& payload) {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char raw : payload) {
        if (std::isalnum(raw) != 0) {
            current.push_back(static_cast<char>(std::tolower(raw)));
            continue;
        }
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
        if (raw > 0x7FU) {
            tokens.emplace_back(1, static_cast<char>(raw));
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    if (tokens.empty() && !payload.empty()) {
        tokens.push_back(payload);
    }
    return tokens;
}

std::vector<float> TokenizedPayloadVector(const std::string& payload, std::size_t dimensions) {
    std::vector<float> values(dimensions, 0.0F);
    const auto tokens = TokenizeText(payload);
    const std::hash<std::string> hasher;
    for (std::size_t token_index = 0; token_index < tokens.size(); ++token_index) {
        const auto& token = tokens[token_index];
        const auto primary_hash = hasher(token);
        const auto secondary_hash = hasher(token + "#" + std::to_string(token_index));
        const auto dimension = primary_hash % dimensions;
        const auto sign = (secondary_hash & 1U) == 0 ? 1.0F : -1.0F;
        const auto weight = 1.0F / std::sqrt(static_cast<float>(1 + token_index));
        values[dimension] += sign * weight;
        values[(dimension + 7) % dimensions] += sign * 0.25F;
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
using Vocabulary = std::unordered_map<std::string, std::int64_t>;

Vocabulary LoadVocabulary(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open tokenizer vocabulary: " + path);
    }

    Vocabulary vocabulary;
    std::string token;
    std::int64_t token_id = 0;
    while (std::getline(input, token)) {
        if (!token.empty() && token.back() == '\r') {
            token.pop_back();
        }
        vocabulary.emplace(token, token_id++);
    }
    if (vocabulary.empty()) {
        throw std::runtime_error("tokenizer vocabulary is empty: " + path);
    }
    return vocabulary;
}

std::int64_t RequireTokenId(const Vocabulary& vocabulary, const std::string& token) {
    const auto iterator = vocabulary.find(token);
    if (iterator == vocabulary.end()) {
        throw std::runtime_error("tokenizer vocabulary is missing required token: " + token);
    }
    return iterator->second;
}

std::vector<std::string> Utf8Tokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::string ascii;
    for (std::size_t index = 0; index < text.size();) {
        const auto raw = static_cast<unsigned char>(text[index]);
        if (raw < 0x80U) {
            if (std::isalnum(raw) != 0) {
                ascii.push_back(static_cast<char>(std::tolower(raw)));
            } else {
                if (!ascii.empty()) {
                    tokens.push_back(ascii);
                    ascii.clear();
                }
                if (std::isspace(raw) == 0) {
                    tokens.emplace_back(1, static_cast<char>(raw));
                }
            }
            ++index;
            continue;
        }

        if (!ascii.empty()) {
            tokens.push_back(ascii);
            ascii.clear();
        }

        std::size_t length = 1;
        if ((raw & 0xE0U) == 0xC0U) {
            length = 2;
        } else if ((raw & 0xF0U) == 0xE0U) {
            length = 3;
        } else if ((raw & 0xF8U) == 0xF0U) {
            length = 4;
        }
        if (index + length > text.size()) {
            tokens.emplace_back(1, text[index]);
            break;
        }
        tokens.push_back(text.substr(index, length));
        index += length;
    }
    if (!ascii.empty()) {
        tokens.push_back(ascii);
    }
    return tokens;
}

std::vector<std::string> BasicWordPieceTokens(const std::string& text) {
    return Utf8Tokens(text);
}

std::vector<std::int64_t> WordPieceTokenIds(const std::string& text,
                                            const Vocabulary& vocabulary,
                                            std::int64_t unknown_token_id,
                                            std::size_t max_piece_count) {
    std::vector<std::int64_t> token_ids;
    for (const auto& token : BasicWordPieceTokens(text)) {
        if (token.size() > 100) {
            token_ids.push_back(unknown_token_id);
            continue;
        }

        std::vector<std::int64_t> pieces;
        std::size_t start = 0;
        bool failed = false;
        while (start < token.size()) {
            std::size_t end = token.size();
            auto found = vocabulary.end();
            while (end > start) {
                std::string candidate = token.substr(start, end - start);
                if (start != 0) {
                    candidate = "##" + candidate;
                }
                found = vocabulary.find(candidate);
                if (found != vocabulary.end()) {
                    break;
                }
                --end;
            }
            if (found == vocabulary.end()) {
                failed = true;
                break;
            }
            pieces.push_back(found->second);
            start = end;
        }

        if (failed) {
            token_ids.push_back(unknown_token_id);
        } else {
            token_ids.insert(token_ids.end(), pieces.begin(), pieces.end());
        }
        if (token_ids.size() >= max_piece_count) {
            token_ids.resize(max_piece_count);
            break;
        }
    }
    return token_ids;
}

std::string NormalizeImagePath(std::string image_reference) {
    constexpr std::string_view kFilePrefix = "file://";
    if (image_reference.rfind(std::string(kFilePrefix), 0) == 0) {
        image_reference.erase(0, kFilePrefix.size());
    }
    return image_reference;
}

std::vector<float> LoadClipImagePixels(const std::string& image_reference) {
    const auto path = NormalizeImagePath(image_reference);
    int width = 0;
    int height = 0;
    int channels = 0;
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> image(
        stbi_load(path.c_str(), &width, &height, &channels, 3), stbi_image_free);
    if (!image || width <= 0 || height <= 0) {
        throw std::runtime_error("failed to load image: " + image_reference);
    }

    constexpr int kTarget = 224;
    constexpr float kMean[3] = {0.48145466F, 0.4578275F, 0.40821073F};
    constexpr float kStd[3] = {0.26862954F, 0.26130258F, 0.27577711F};
    std::vector<float> pixels(3 * kTarget * kTarget, 0.0F);

    const auto source_width = static_cast<float>(width);
    const auto source_height = static_cast<float>(height);
    for (int y = 0; y < kTarget; ++y) {
        const float source_y = std::clamp(
            (static_cast<float>(y) + 0.5F) * source_height / static_cast<float>(kTarget) - 0.5F,
            0.0F,
            source_height - 1.0F);
        const int y0 = std::clamp(static_cast<int>(std::floor(source_y)), 0, height - 1);
        const int y1 = std::clamp(y0 + 1, 0, height - 1);
        const float y_weight = source_y - static_cast<float>(y0);
        for (int x = 0; x < kTarget; ++x) {
            const float source_x = std::clamp(
                (static_cast<float>(x) + 0.5F) * source_width / static_cast<float>(kTarget) - 0.5F,
                0.0F,
                source_width - 1.0F);
            const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0, width - 1);
            const int x1 = std::clamp(x0 + 1, 0, width - 1);
            const float x_weight = source_x - static_cast<float>(x0);

            for (int channel = 0; channel < 3; ++channel) {
                const auto sample = [&](int px, int py) {
                    return static_cast<float>(image.get()[(py * width + px) * 3 + channel]);
                };
                const float top = sample(x0, y0) * (1.0F - x_weight) + sample(x1, y0) * x_weight;
                const float bottom = sample(x0, y1) * (1.0F - x_weight) + sample(x1, y1) * x_weight;
                const float value = (top * (1.0F - y_weight) + bottom * y_weight) / 255.0F;
                pixels[channel * kTarget * kTarget + y * kTarget + x] = (value - kMean[channel]) / kStd[channel];
            }
        }
    }
    return pixels;
}

class OnnxRuntimeEmbeddingService final : public EmbeddingService {
public:
    explicit OnnxRuntimeEmbeddingService(const kvai::infra::ServerConfig& config)
        : env_(ORT_LOGGING_LEVEL_WARNING, "rangeveckv"),
          session_options_(),
          dimensions_(config.embedding_dimensions),
          max_tokens_(config.ai_max_tokens),
          vocabulary_(LoadVocabulary(config.tokenizer_path)),
          cls_token_id_(RequireTokenId(vocabulary_, "[CLS]")),
          separator_token_id_(RequireTokenId(vocabulary_, "[SEP]")),
          unknown_token_id_(RequireTokenId(vocabulary_, "[UNK]")) {
        if (max_tokens_ < 2) {
            throw std::runtime_error("ai.max_tokens must be at least 2");
        }
        session_options_.SetIntraOpNumThreads(1);
        session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_ = std::make_unique<Ort::Session>(env_, config.model_path.c_str(), session_options_);
        ValidateModelInputs();
    }

    kvai::infra::StatusOr<Embedding> EmbedText(const std::string& text) const override {
        if (text.empty()) {
            return kvai::infra::Status::InvalidArgument("text query cannot be empty");
        }
        return EmbedTextPayload(text);
    }

    kvai::infra::StatusOr<Embedding> EmbedImage(const std::string& image_reference) const override {
        if (image_reference.empty()) {
            return kvai::infra::Status::InvalidArgument("image reference cannot be empty");
        }
        if (!is_clip_model_) {
            return kvai::infra::Status::NotSupported("the configured ONNX text embedding model does not support image input");
        }
        try {
            return EmbedClip(image_reference, "image_embeds", "onnxruntime-chinese-clip-image", false);
        } catch (const std::exception& error) {
            return kvai::infra::Status::Unavailable(std::string("image embedding failed: ") + error.what());
        }
    }

private:
    void ValidateModelInputs() {
        Ort::AllocatorWithDefaultOptions allocator;
        for (std::size_t index = 0; index < session_->GetInputCount(); ++index) {
            auto name = session_->GetInputNameAllocated(index, allocator);
            input_names_.emplace(name.get());
        }
        for (std::size_t index = 0; index < session_->GetOutputCount(); ++index) {
            auto name = session_->GetOutputNameAllocated(index, allocator);
            output_names_.emplace(name.get());
        }

        is_clip_model_ = input_names_.count("pixel_values") != 0 && output_names_.count("text_embeds") != 0 &&
                         output_names_.count("image_embeds") != 0;

        for (const auto* required : {"input_ids", "attention_mask"}) {
            if (input_names_.find(required) == input_names_.end()) {
                throw std::runtime_error(std::string("ONNX model is missing required input: ") + required);
            }
        }
        if (!is_clip_model_ && input_names_.find("token_type_ids") == input_names_.end()) {
            throw std::runtime_error("ONNX model is missing required input: token_type_ids");
        }
    }

    kvai::infra::StatusOr<Embedding> EmbedTextPayload(const std::string& text) const {
        if (is_clip_model_) {
            try {
                return EmbedClip(text, "text_embeds", "onnxruntime-chinese-clip-text", true);
            } catch (const std::exception& error) {
                return kvai::infra::Status::Unavailable(std::string("text embedding failed: ") + error.what());
            }
        }

        if (dimensions_ == 0) {
            return kvai::infra::Status::InvalidArgument("embedding dimensions must be positive");
        }

        auto input_ids = WordPieceTokenIds(text, vocabulary_, unknown_token_id_, max_tokens_ - 2);
        input_ids.insert(input_ids.begin(), cls_token_id_);
        input_ids.push_back(separator_token_id_);

        std::vector<std::int64_t> attention_mask(input_ids.size(), 1);
        std::vector<std::int64_t> token_type_ids(input_ids.size(), 0);
        std::vector<std::int64_t> shape{1, static_cast<std::int64_t>(input_ids.size())};
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> input_tensors;
        input_tensors.emplace_back(Ort::Value::CreateTensor<std::int64_t>(
            memory_info, input_ids.data(), input_ids.size(), shape.data(), shape.size()));
        input_tensors.emplace_back(Ort::Value::CreateTensor<std::int64_t>(
            memory_info, attention_mask.data(), attention_mask.size(), shape.data(), shape.size()));
        input_tensors.emplace_back(Ort::Value::CreateTensor<std::int64_t>(
            memory_info, token_type_ids.data(), token_type_ids.size(), shape.data(), shape.size()));

        Ort::AllocatorWithDefaultOptions allocator;
        auto output_name = session_->GetOutputNameAllocated(0, allocator);
        const char* input_names[] = {"input_ids", "attention_mask", "token_type_ids"};
        const char* output_names[] = {output_name.get()};

        try {
            auto outputs = session_->Run(
                Ort::RunOptions{nullptr}, input_names, input_tensors.data(), input_tensors.size(), output_names, 1);
            if (outputs.empty() || !outputs.front().IsTensor()) {
                return kvai::infra::Status::Internal("onnxruntime produced no tensor output");
            }

            const auto& output_tensor = outputs.front();
            const auto shape_info = output_tensor.GetTensorTypeAndShapeInfo();
            const auto output_shape = shape_info.GetShape();
            if (output_shape.size() != 3 || output_shape[0] != 1 ||
                output_shape[1] != static_cast<std::int64_t>(input_ids.size()) || output_shape[2] <= 0) {
                return kvai::infra::Status::Internal("onnxruntime produced an unexpected text embedding tensor shape");
            }

            const auto hidden_dimensions = static_cast<std::size_t>(output_shape[2]);
            if (hidden_dimensions != dimensions_) {
                return kvai::infra::Status::InvalidArgument(
                    "ai.embedding_dimensions does not match ONNX model output dimensions");
            }
            const float* output_data = output_tensor.GetTensorData<float>();

            Embedding embedding;
            embedding.backend = "onnxruntime-minilm-text";
            embedding.values.assign(hidden_dimensions, 0.0F);
            for (std::size_t token_index = 0; token_index < input_ids.size(); ++token_index) {
                const auto offset = token_index * hidden_dimensions;
                for (std::size_t dimension = 0; dimension < hidden_dimensions; ++dimension) {
                    embedding.values[dimension] += output_data[offset + dimension];
                }
            }
            const auto token_count = static_cast<float>(input_ids.size());
            for (float& value : embedding.values) {
                value /= token_count;
            }
            auto status = Normalize(embedding.values);
            if (!status.ok()) {
                return status;
            }
            return embedding;
        } catch (const Ort::Exception& error) {
            return kvai::infra::Status::Unavailable(std::string("onnxruntime inference failed: ") + error.what());
        }
    }

    std::vector<std::int64_t> BuildInputIds(const std::string& text) const {
        auto input_ids = WordPieceTokenIds(text, vocabulary_, unknown_token_id_, max_tokens_ - 2);
        input_ids.insert(input_ids.begin(), cls_token_id_);
        input_ids.push_back(separator_token_id_);
        return input_ids;
    }

    kvai::infra::StatusOr<Embedding> EmbedClip(const std::string& payload,
                                               const char* output_name,
                                               const char* backend,
                                               bool payload_is_text) const {
        if (dimensions_ == 0) {
            return kvai::infra::Status::InvalidArgument("embedding dimensions must be positive");
        }

        auto input_ids = payload_is_text ? BuildInputIds(payload) : BuildInputIds("image");
        std::vector<std::int64_t> attention_mask(input_ids.size(), 1);
        std::vector<std::int64_t> text_shape{1, static_cast<std::int64_t>(input_ids.size())};

        std::vector<float> pixel_values;
        if (payload_is_text) {
            pixel_values.assign(3 * 224 * 224, 0.0F);
        } else {
            pixel_values = LoadClipImagePixels(payload);
        }
        std::vector<std::int64_t> pixel_shape{1, 3, 224, 224};

        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> input_tensors;
        input_tensors.emplace_back(Ort::Value::CreateTensor<std::int64_t>(
            memory_info, input_ids.data(), input_ids.size(), text_shape.data(), text_shape.size()));
        input_tensors.emplace_back(Ort::Value::CreateTensor<float>(
            memory_info, pixel_values.data(), pixel_values.size(), pixel_shape.data(), pixel_shape.size()));
        input_tensors.emplace_back(Ort::Value::CreateTensor<std::int64_t>(
            memory_info, attention_mask.data(), attention_mask.size(), text_shape.data(), text_shape.size()));

        const char* input_names[] = {"input_ids", "pixel_values", "attention_mask"};
        const char* output_names[] = {output_name};

        try {
            auto outputs = session_->Run(
                Ort::RunOptions{nullptr}, input_names, input_tensors.data(), input_tensors.size(), output_names, 1);
            if (outputs.empty() || !outputs.front().IsTensor()) {
                return kvai::infra::Status::Internal("onnxruntime produced no CLIP tensor output");
            }

            const auto& output_tensor = outputs.front();
            const auto shape = output_tensor.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 2 || shape[0] != 1 || shape[1] <= 0) {
                return kvai::infra::Status::Internal("onnxruntime produced an unexpected CLIP embedding tensor shape");
            }
            const auto output_dimensions = static_cast<std::size_t>(shape[1]);
            if (output_dimensions != dimensions_) {
                return kvai::infra::Status::InvalidArgument(
                    "ai.embedding_dimensions does not match ONNX CLIP output dimensions");
            }

            const float* output_data = output_tensor.GetTensorData<float>();
            Embedding embedding;
            embedding.backend = backend;
            embedding.values.assign(output_data, output_data + output_dimensions);
            auto status = Normalize(embedding.values);
            if (!status.ok()) {
                return status;
            }
            return embedding;
        } catch (const Ort::Exception& error) {
            return kvai::infra::Status::Unavailable(std::string("onnxruntime CLIP inference failed: ") + error.what());
        }
    }

    Ort::Env env_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    std::size_t dimensions_;
    std::size_t max_tokens_;
    Vocabulary vocabulary_;
    std::int64_t cls_token_id_;
    std::int64_t separator_token_id_;
    std::int64_t unknown_token_id_;
    bool is_clip_model_ = false;
    std::unordered_set<std::string> input_names_;
    std::unordered_set<std::string> output_names_;
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
    embedding.values = TokenizedPayloadVector(payload, dimensions_);
    embedding.backend = backend;
    auto status = Normalize(embedding.values);
    if (!status.ok()) {
        return status;
    }
    return embedding;
}

kvai::infra::StatusOr<std::unique_ptr<EmbeddingService>> CreateEmbeddingService(const kvai::infra::ServerConfig& config) {
#if defined(KVAI_HAVE_ONNXRUNTIME)
    std::string initialization_error;
    if (config.ai_backend == "onnxruntime" || config.ai_backend == "auto") {
        if (!config.model_path.empty()) {
            namespace fs = std::filesystem;
            if (fs::exists(config.model_path)) {
                try {
                    kvai::infra::log::Info("ai", "initializing ONNX Runtime embedding service", {{"model_path", config.model_path}});
                    return std::unique_ptr<EmbeddingService>(std::make_unique<OnnxRuntimeEmbeddingService>(config));
                } catch (const std::exception& error) {
                    initialization_error = error.what();
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
            if (!initialization_error.empty()) {
                return kvai::infra::Status::Unavailable(
                    "onnxruntime backend initialization failed: " + initialization_error);
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
