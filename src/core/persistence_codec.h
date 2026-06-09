#pragma once

#include <string>
#include <vector>

#include "core/document.h"
#include "infra/status.h"

namespace kvai::core::persistence {

enum class WalOperation {
    kPut = 1,
    kDelete = 2,
};

struct WalEntry {
    WalOperation operation = WalOperation::kPut;
    DocumentRecord record;
    std::string collection;
    std::string key;
};

struct VectorEntry {
    DocumentRecord record;
    std::vector<float> vector;
};

[[nodiscard]] const std::string& MagicHeader();

[[nodiscard]] std::string EncodeDocumentRecord(const DocumentRecord& record);
[[nodiscard]] kvai::infra::StatusOr<DocumentRecord> DecodeDocumentRecord(const std::string& payload);

[[nodiscard]] std::string EncodeWalEntry(const WalEntry& entry);
[[nodiscard]] kvai::infra::StatusOr<WalEntry> DecodeWalEntry(const std::string& payload);

[[nodiscard]] std::string EncodeVectorEntry(const VectorEntry& entry);
[[nodiscard]] kvai::infra::StatusOr<VectorEntry> DecodeVectorEntry(const std::string& payload);

[[nodiscard]] std::string EncodeFrame(const std::string& payload);
[[nodiscard]] kvai::infra::StatusOr<std::vector<std::string>> DecodeFrames(const std::string& bytes);

}  // namespace kvai::core::persistence
