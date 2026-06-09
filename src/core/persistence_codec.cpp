#include "core/persistence_codec.h"

#include <cstdint>

#include "search.pb.h"

namespace kvai::core::persistence {

namespace {

kvai::v1::DocumentRecord ToProto(const DocumentRecord& record) {
    kvai::v1::DocumentRecord proto;
    proto.set_collection(record.collection);
    proto.set_key(record.key);
    proto.set_title(record.title);
    proto.set_body(record.body);
    auto* metadata = proto.mutable_metadata();
    for (const auto& [key, value] : record.metadata) {
        (*metadata)[key] = value;
    }
    return proto;
}

DocumentRecord FromProto(const kvai::v1::DocumentRecord& proto) {
    DocumentRecord record;
    record.collection = proto.collection();
    record.key = proto.key();
    record.title = proto.title();
    record.body = proto.body();
    for (const auto& [key, value] : proto.metadata()) {
        record.metadata[key] = value;
    }
    return record;
}

kvai::v1::WalOperation ToProto(WalOperation operation) {
    switch (operation) {
    case WalOperation::kPut:
        return kvai::v1::WAL_OPERATION_PUT;
    case WalOperation::kDelete:
        return kvai::v1::WAL_OPERATION_DELETE;
    }
    return kvai::v1::WAL_OPERATION_UNSPECIFIED;
}

WalOperation FromProto(kvai::v1::WalOperation operation) {
    return operation == kvai::v1::WAL_OPERATION_DELETE ? WalOperation::kDelete : WalOperation::kPut;
}

}  // namespace

const std::string& MagicHeader() {
    static const std::string header = "KVAI-PB1\n";
    return header;
}

std::string EncodeDocumentRecord(const DocumentRecord& record) {
    return ToProto(record).SerializeAsString();
}

kvai::infra::StatusOr<DocumentRecord> DecodeDocumentRecord(const std::string& payload) {
    kvai::v1::DocumentRecord proto;
    if (!proto.ParseFromString(payload)) {
        return kvai::infra::Status::Internal("failed to parse protobuf DocumentRecord");
    }
    return FromProto(proto);
}

std::string EncodeWalEntry(const WalEntry& entry) {
    kvai::v1::WalEntry proto;
    proto.set_operation(ToProto(entry.operation));
    if (entry.operation == WalOperation::kPut) {
        *proto.mutable_record() = ToProto(entry.record);
    } else {
        proto.set_collection(entry.collection);
        proto.set_key(entry.key);
    }
    return proto.SerializeAsString();
}

kvai::infra::StatusOr<WalEntry> DecodeWalEntry(const std::string& payload) {
    kvai::v1::WalEntry proto;
    if (!proto.ParseFromString(payload)) {
        return kvai::infra::Status::Internal("failed to parse protobuf WalEntry");
    }

    WalEntry entry;
    entry.operation = FromProto(proto.operation());
    if (entry.operation == WalOperation::kPut) {
        entry.record = FromProto(proto.record());
    } else {
        entry.collection = proto.collection();
        entry.key = proto.key();
    }
    return entry;
}

std::string EncodeVectorEntry(const VectorEntry& entry) {
    kvai::v1::VectorEntry proto;
    *proto.mutable_record() = ToProto(entry.record);
    for (float value : entry.vector) {
        proto.add_vector(value);
    }
    return proto.SerializeAsString();
}

kvai::infra::StatusOr<VectorEntry> DecodeVectorEntry(const std::string& payload) {
    kvai::v1::VectorEntry proto;
    if (!proto.ParseFromString(payload)) {
        return kvai::infra::Status::Internal("failed to parse protobuf VectorEntry");
    }

    VectorEntry entry;
    entry.record = FromProto(proto.record());
    entry.vector.assign(proto.vector().begin(), proto.vector().end());
    return entry;
}

std::string EncodeFrame(const std::string& payload) {
    std::string output;
    const auto length = static_cast<std::uint32_t>(payload.size());
    for (int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<char>((length >> shift) & 0xFFU));
    }
    output.append(payload);
    return output;
}

kvai::infra::StatusOr<std::vector<std::string>> DecodeFrames(const std::string& bytes) {
    std::vector<std::string> frames;
    if (bytes.rfind(MagicHeader(), 0) != 0) {
        return kvai::infra::Status::InvalidArgument("missing protobuf persistence magic header");
    }
    std::size_t offset = MagicHeader().size();
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 4) {
            return kvai::infra::Status::Internal("truncated protobuf persistence frame");
        }
        std::uint32_t length = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            length |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset++])) << shift;
        }
        if (length > bytes.size() - offset) {
            return kvai::infra::Status::Internal("truncated protobuf persistence payload");
        }
        frames.push_back(bytes.substr(offset, length));
        offset += length;
    }
    return frames;
}

}  // namespace kvai::core::persistence
