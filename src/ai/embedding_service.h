#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "infra/config.h"
#include "infra/status.h"

namespace kvai::ai {

struct Embedding {
    std::vector<float> values;
    std::string backend;
    bool degraded = false;
};

class EmbeddingService {
public:
    virtual ~EmbeddingService() = default;

    virtual kvai::infra::StatusOr<Embedding> EmbedText(const std::string& text) const = 0;
    virtual kvai::infra::StatusOr<Embedding> EmbedImage(const std::string& image_reference) const = 0;
};

class DeterministicEmbeddingService final : public EmbeddingService {
public:
    explicit DeterministicEmbeddingService(std::size_t dimensions);

    kvai::infra::StatusOr<Embedding> EmbedText(const std::string& text) const override;
    kvai::infra::StatusOr<Embedding> EmbedImage(const std::string& image_reference) const override;

private:
    kvai::infra::StatusOr<Embedding> EmbedPayload(const std::string& payload, const std::string& backend) const;

    std::size_t dimensions_;
};

kvai::infra::StatusOr<std::unique_ptr<EmbeddingService>> CreateEmbeddingService(const kvai::infra::ServerConfig& config);

}  // namespace kvai::ai