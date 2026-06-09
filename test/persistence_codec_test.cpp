#include <cmath>
#include <iostream>

#include "core/persistence_codec.h"

namespace {

bool Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    int failures = 0;

    kvai::core::DocumentRecord record{"documents", "doc-1", "Title", "Body text", {{"lang", "cpp"}, {"domain", "search"}}};
    const auto encoded_record = kvai::core::persistence::EncodeDocumentRecord(record);
    auto decoded_record = kvai::core::persistence::DecodeDocumentRecord(encoded_record);
    if (!Expect(decoded_record.ok(), "document decode failed")) ++failures;
    if (decoded_record.ok()) {
        if (!Expect(decoded_record.value().collection == record.collection, "collection mismatch")) ++failures;
        if (!Expect(decoded_record.value().metadata == record.metadata, "metadata mismatch")) ++failures;
    }

    kvai::core::persistence::WalEntry put;
    put.operation = kvai::core::persistence::WalOperation::kPut;
    put.record = record;
    auto decoded_put = kvai::core::persistence::DecodeWalEntry(kvai::core::persistence::EncodeWalEntry(put));
    if (!Expect(decoded_put.ok(), "wal put decode failed")) ++failures;
    if (decoded_put.ok()) {
        if (!Expect(decoded_put.value().operation == kvai::core::persistence::WalOperation::kPut, "wal put operation mismatch")) ++failures;
        if (!Expect(decoded_put.value().record.key == "doc-1", "wal put record mismatch")) ++failures;
    }

    kvai::core::persistence::WalEntry del;
    del.operation = kvai::core::persistence::WalOperation::kDelete;
    del.collection = "documents";
    del.key = "doc-1";
    auto decoded_delete = kvai::core::persistence::DecodeWalEntry(kvai::core::persistence::EncodeWalEntry(del));
    if (!Expect(decoded_delete.ok(), "wal delete decode failed")) ++failures;
    if (decoded_delete.ok()) {
        if (!Expect(decoded_delete.value().collection == "documents", "wal delete collection mismatch")) ++failures;
        if (!Expect(decoded_delete.value().key == "doc-1", "wal delete key mismatch")) ++failures;
    }

    kvai::core::persistence::VectorEntry vector_entry{record, {0.25F, -0.5F, 1.0F}};
    auto decoded_vector = kvai::core::persistence::DecodeVectorEntry(kvai::core::persistence::EncodeVectorEntry(vector_entry));
    if (!Expect(decoded_vector.ok(), "vector decode failed")) ++failures;
    if (decoded_vector.ok()) {
        if (!Expect(decoded_vector.value().record.key == "doc-1", "vector record mismatch")) ++failures;
        if (!Expect(decoded_vector.value().vector.size() == 3, "vector size mismatch")) ++failures;
        if (!Expect(std::fabs(decoded_vector.value().vector[1] + 0.5F) < 0.0001F, "vector value mismatch")) ++failures;
    }

    std::string framed = kvai::core::persistence::MagicHeader();
    framed += kvai::core::persistence::EncodeFrame(encoded_record);
    framed += kvai::core::persistence::EncodeFrame(encoded_record);
    auto frames = kvai::core::persistence::DecodeFrames(framed);
    if (!Expect(frames.ok(), "frame decode failed")) ++failures;
    if (frames.ok()) {
        if (!Expect(frames.value().size() == 2, "frame count mismatch")) ++failures;
    }

    return failures == 0 ? 0 : 1;
}
