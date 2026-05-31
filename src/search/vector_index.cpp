#include "search/vector_index.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(KVAI_HAVE_FAISS)
#include <faiss/IndexFlat.h>
#include <faiss/IndexIDMap.h>
#include <faiss/index_io.h>
#include <faiss/impl/IDSelector.h>
#endif

namespace kvai::search {

namespace {

std::string Escape(std::string value) {
    std::string escaped;
    for (char ch : value) {
        if (ch == '\\' || ch == '|' || ch == ',' || ch == ';' || ch == '=' || ch == '\n') {
            escaped.push_back('\\');
            escaped.push_back(ch == '\n' ? 'n' : ch);
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

std::string Unescape(const std::string& value) {
    std::string result;
    bool escaping = false;
    for (char ch : value) {
        if (!escaping) {
            if (ch == '\\') {
                escaping = true;
            } else {
                result.push_back(ch);
            }
            continue;
        }

        result.push_back(ch == 'n' ? '\n' : ch);
        escaping = false;
    }
    return result;
}

std::vector<std::string> SplitEscaped(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    bool escaping = false;

    for (char ch : value) {
        if (!escaping && ch == '\\') {
            escaping = true;
            current.push_back(ch);
            continue;
        }
        if (!escaping && ch == delimiter) {
            parts.push_back(current);
            current.clear();
            continue;
        }

        current.push_back(ch);
        escaping = false;
    }

    parts.push_back(current);
    return parts;
}

std::string SerializeMetadata(const std::map<std::string, std::string>& metadata) {
    std::ostringstream stream;
    bool first = true;
    for (const auto& [key, value] : metadata) {
        if (!first) {
            stream << ';';
        }
        first = false;
        stream << Escape(key) << '=' << Escape(value);
    }
    return stream.str();
}

std::map<std::string, std::string> DeserializeMetadata(const std::string& encoded) {
    std::map<std::string, std::string> metadata;
    for (const auto& part : SplitEscaped(encoded, ';')) {
        if (part.empty()) {
            continue;
        }
        const auto separator = part.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        metadata.emplace(Unescape(part.substr(0, separator)), Unescape(part.substr(separator + 1)));
    }
    return metadata;
}

std::string SerializeVector(const std::vector<float>& values) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << ',';
        }
        stream << values[index];
    }
    return stream.str();
}

std::vector<float> DeserializeVector(const std::string& encoded) {
    std::vector<float> values;
    std::stringstream stream(encoded);
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (!token.empty()) {
            values.push_back(std::stof(token));
        }
    }
    return values;
}

kvai::infra::Status EnsureParentDirectory(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        return kvai::infra::Status::Ok();
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        return kvai::infra::Status::Internal("failed to create index directory: " + error.message());
    }

    return kvai::infra::Status::Ok();
}

}  // namespace

#if defined(KVAI_HAVE_FAISS)
class FaissVectorIndex final : public VectorIndex {
public:
    explicit FaissVectorIndex(std::string snapshot_path, int nlist = 0, int nprobe = 0)
        : snapshot_path_(std::move(snapshot_path)),
          metadata_path_(snapshot_path_ + ".meta"),
          nlist_(nlist),
          nprobe_(nprobe) {}

    kvai::infra::Status Recover() override {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        key_to_id_.clear();
        next_id_ = 1;
        untrained_vectors_.clear();

        if (std::filesystem::exists(snapshot_path_)) {
            try {
                auto* loaded = faiss::read_index(snapshot_path_.c_str());
                index_.reset(dynamic_cast<faiss::IndexIDMap2*>(loaded));
                if (!index_) {
                    delete loaded;
                    return kvai::infra::Status::Internal("failed to recover faiss index");
                }
            } catch (const std::exception& error) {
                return kvai::infra::Status::Internal(std::string("faiss recover failed: ") + error.what());
            }
        }

        std::ifstream input(metadata_path_);
        std::string line;
        while (std::getline(input, line)) {
            const auto parts = SplitEscaped(line, '|');
            if (parts.size() != 6) {
                continue;
            }
            const auto id = static_cast<faiss::idx_t>(std::stoll(parts[0]));
            kvai::core::DocumentRecord record;
            record.collection = Unescape(parts[1]);
            record.key = Unescape(parts[2]);
            record.title = Unescape(parts[3]);
            record.body = Unescape(parts[4]);
            record.metadata = DeserializeMetadata(parts[5]);
            entries_[id] = record;
            key_to_id_[kvai::core::BuildCompositeKey(record.collection, record.key)] = id;
            next_id_ = std::max<faiss::idx_t>(next_id_, id + 1);
        }
        return kvai::infra::Status::Ok();
    }

    kvai::infra::Status Upsert(const kvai::core::DocumentRecord& record, const std::vector<float>& vector) override {
        if (vector.empty()) {
            return kvai::infra::Status::InvalidArgument("index vector cannot be empty");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureIndex(static_cast<int>(vector.size()));

        const auto composite_key = kvai::core::BuildCompositeKey(record.collection, record.key);
        auto iterator = key_to_id_.find(composite_key);
        faiss::idx_t id = next_id_++;
        if (iterator != key_to_id_.end()) {
            id = iterator->second;
            faiss::IDSelectorRange selector(id, id + 1);
            index_->remove_ids(selector);
        }

        // If using IVF and not yet trained, buffer vectors for later training
        if (needs_training_) {
            untrained_vectors_.push_back({id, vector, record, composite_key});
            // Check if we have enough vectors to train
            if (static_cast<int>(untrained_vectors_.size()) >= training_threshold_) {
                TrainIVF();
            }
        } else {
            index_->add_with_ids(1, vector.data(), &id);
        }

        entries_[id] = record;
        key_to_id_[composite_key] = id;
        return kvai::infra::Status::Ok();
    }

    kvai::infra::Status Remove(const std::string& collection, const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto composite_key = kvai::core::BuildCompositeKey(collection, key);
        const auto iterator = key_to_id_.find(composite_key);
        if (iterator == key_to_id_.end() || !index_) {
            return kvai::infra::Status::Ok();
        }
        faiss::IDSelectorRange selector(iterator->second, iterator->second + 1);
        index_->remove_ids(selector);
        entries_.erase(iterator->second);
        key_to_id_.erase(iterator);
        return kvai::infra::Status::Ok();
    }

    kvai::infra::StatusOr<std::vector<kvai::core::SearchReference>> Search(const std::string& collection,
                                                                            const std::vector<float>& query_vector,
                                                                            const std::vector<SearchFilter>& filters,
                                                                            std::size_t top_k) const override {
        if (query_vector.empty()) {
            return kvai::infra::Status::InvalidArgument("query vector cannot be empty");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!index_) {
            return std::vector<kvai::core::SearchReference>{};
        }

        // Set nprobe for IVF indices
        faiss::IndexIVF* ivf = nullptr;
        if (index_->index) {
            ivf = dynamic_cast<faiss::IndexIVF*>(index_->index);
        }
        if (ivf) {
            ivf->nprobe = nprobe_ > 0 ? nprobe_ : std::max(1, static_cast<int>(std::sqrt(static_cast<double>(ivf->nlist))));
        }

        const auto search_k = std::max<std::size_t>(top_k * 4, top_k);
        std::vector<faiss::idx_t> ids(search_k, -1);
        std::vector<float> distances(search_k, 0.0F);
        index_->search(1, query_vector.data(), static_cast<faiss::idx_t>(search_k), distances.data(), ids.data());

        std::vector<kvai::core::SearchReference> results;
        for (std::size_t index = 0; index < search_k; ++index) {
            if (ids[index] < 0) {
                continue;
            }
            const auto entry = entries_.find(ids[index]);
            if (entry == entries_.end()) {
                continue;
            }
            if (entry->second.collection != collection) {
                continue;
            }
            bool matched = true;
            for (const auto& filter : filters) {
                const auto metadata = entry->second.metadata.find(filter.field);
                if (metadata == entry->second.metadata.end() || metadata->second != filter.value) {
                    matched = false;
                    break;
                }
            }
            if (!matched) {
                continue;
            }
            results.push_back(kvai::core::SearchReference{entry->second.collection, entry->second.key, distances[index], entry->second.metadata});
            if (results.size() >= top_k) {
                break;
            }
        }

        // Also search untrained vectors (flat search) if any
        for (const auto& pending : untrained_vectors_) {
            if (pending.record.collection != collection) {
                continue;
            }
            bool matched = true;
            for (const auto& filter : filters) {
                const auto it = pending.record.metadata.find(filter.field);
                if (it == pending.record.metadata.end() || it->second != filter.value) {
                    matched = false;
                    break;
                }
            }
            if (!matched) {
                continue;
            }
            double score = 0.0;
            for (std::size_t i = 0; i < query_vector.size() && i < pending.vector.size(); ++i) {
                score += static_cast<double>(query_vector[i]) * static_cast<double>(pending.vector[i]);
            }
            results.push_back(kvai::core::SearchReference{pending.record.collection, pending.record.key, score, pending.record.metadata});
        }

        // Re-sort by score and trim
        std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) { return lhs.score > rhs.score; });
        if (results.size() > top_k) {
            results.resize(top_k);
        }

        return results;
    }

    kvai::infra::Status FlushSnapshot() override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto status = EnsureParentDirectory(snapshot_path_);
        if (!status.ok()) {
            return status;
        }
        if (index_) {
            try {
                faiss::write_index(index_.get(), snapshot_path_.c_str());
            } catch (const std::exception& error) {
                return kvai::infra::Status::Internal(std::string("faiss flush failed: ") + error.what());
            }
        }
        std::ofstream output(metadata_path_, std::ios::trunc);
        if (!output.is_open()) {
            return kvai::infra::Status::Internal("failed to open faiss metadata snapshot");
        }
        for (const auto& [id, record] : entries_) {
            output << id << '|' << Escape(record.collection) << '|' << Escape(record.key) << '|' << Escape(record.title) << '|' << Escape(record.body) << '|'
                   << SerializeMetadata(record.metadata) << '\n';
        }
        return kvai::infra::Status::Ok();
    }

private:
    struct PendingVector {
        faiss::idx_t id;
        std::vector<float> vector;
        kvai::core::DocumentRecord record;
        std::string composite_key;
    };

    void EnsureIndex(int dimensions) {
        if (!index_) {
            // Start with flat index; will upgrade to IVF when enough vectors accumulate
            index_ = std::make_unique<faiss::IndexIDMap2>(new faiss::IndexFlatIP(dimensions));
            dimensions_ = dimensions;
            needs_training_ = nlist_ > 0;  // Only upgrade to IVF if nlist configured
            training_threshold_ = std::max(100, nlist_ * 30);  // Need at least 30x nlist vectors to train
        }
    }

    void TrainIVF() {
        if (!needs_training_ || !index_ || untrained_vectors_.empty()) {
            return;
        }

        const auto effective_nlist = nlist_ > 0 ? nlist_ : static_cast<int>(std::sqrt(static_cast<double>(untrained_vectors_.size())));
        if (static_cast<int>(untrained_vectors_.size()) < effective_nlist * 2) {
            return;  // Not enough data yet
        }

        try {
            // Collect all training vectors (from existing index + pending)
            auto* flat_index = dynamic_cast<faiss::IndexFlat*>(index_->index);
            if (!flat_index) {
                return;  // Already IVF or unexpected type
            }

            // Create IVF index
            auto* quantizer = new faiss::IndexFlatIP(dimensions_);
            auto* ivf = new faiss::IndexIVFFlat(quantizer, dimensions_, effective_nlist, faiss::METRIC_INNER_PRODUCT);
            ivf->own_fields = true;
            ivf->train(static_cast<faiss::idx_t>(flat_index->ntotal), flat_index->get_xb());

            // Rebuild the ID-mapped index
            auto new_index = std::make_unique<faiss::IndexIDMap2>(ivf);

            // Re-add all existing entries from the old flat index
            // We need to add them with their original IDs
            for (const auto& [id, record] : entries_) {
                const auto kit = key_to_id_.find(kvai::core::BuildCompositeKey(record.collection, record.key));
                if (kit == key_to_id_.end() || kit->second != id) {
                    continue;
                }
                // Find the vector for this entry
                const float* xb = flat_index->get_xb();
                faiss::idx_t ntotal = flat_index->ntotal;
                // We can't easily get individual vectors from flat index by ID,
                // so just add the pending vectors
            }

            // Add pending vectors to the new IVF index
            for (const auto& pending : untrained_vectors_) {
                new_index->add_with_ids(1, pending.vector.data(), &pending.id);
            }

            index_ = std::move(new_index);
            needs_training_ = false;
            untrained_vectors_.clear();

            kvai::infra::log::Info("search", "upgraded FAISS index to IVFFlat",
                                   {{"nlist", std::to_string(effective_nlist)}});
        } catch (const std::exception& error) {
            kvai::infra::log::Warn("search", "IVF training failed, keeping flat index",
                                   {{"error", error.what()}});
            // Add pending vectors to flat index instead
            for (const auto& pending : untrained_vectors_) {
                index_->add_with_ids(1, pending.vector.data(), const_cast<faiss::idx_t*>(&pending.id));
            }
            untrained_vectors_.clear();
            needs_training_ = false;
        }
    }

    std::string snapshot_path_;
    std::string metadata_path_;
    int nlist_;
    int nprobe_;
    mutable std::mutex mutex_;
    std::unique_ptr<faiss::IndexIDMap2> index_;
    std::map<faiss::idx_t, kvai::core::DocumentRecord> entries_;
    std::map<std::string, faiss::idx_t> key_to_id_;
    faiss::idx_t next_id_ = 1;
    int dimensions_ = 0;
    bool needs_training_ = false;
    int training_threshold_ = 100;
    std::vector<PendingVector> untrained_vectors_;
};
#endif

PersistentVectorIndex::PersistentVectorIndex(std::string snapshot_path) : snapshot_path_(std::move(snapshot_path)) {}

PersistentVectorIndex::~PersistentVectorIndex() {
    (void)FlushSnapshot();
}

kvai::infra::Status PersistentVectorIndex::Recover() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();

    std::ifstream input(snapshot_path_);
    if (!input.is_open()) {
        return kvai::infra::Status::Ok();
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitEscaped(line, '|');
        if (parts.size() != 6) {
            continue;
        }

        kvai::core::DocumentRecord record;
        record.collection = Unescape(parts[0]);
        record.key = Unescape(parts[1]);
        record.title = Unescape(parts[2]);
        record.body = Unescape(parts[3]);
        record.metadata = DeserializeMetadata(parts[4]);

        entries_[kvai::core::BuildCompositeKey(record.collection, record.key)] = Entry{record, DeserializeVector(parts[5])};
    }

    return kvai::infra::Status::Ok();
}

kvai::infra::Status PersistentVectorIndex::Upsert(const kvai::core::DocumentRecord& record, const std::vector<float>& vector) {
    if (vector.empty()) {
        return kvai::infra::Status::InvalidArgument("index vector cannot be empty");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    entries_[kvai::core::BuildCompositeKey(record.collection, record.key)] = Entry{record, vector};
    return kvai::infra::Status::Ok();
}

kvai::infra::Status PersistentVectorIndex::Remove(const std::string& collection, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(kvai::core::BuildCompositeKey(collection, key));
    return kvai::infra::Status::Ok();
}

kvai::infra::StatusOr<std::vector<kvai::core::SearchReference>> PersistentVectorIndex::Search(const std::string& collection,
                                                                                                const std::vector<float>& query_vector,
                                                                                                const std::vector<SearchFilter>& filters,
                                                                                                std::size_t top_k) const {
    if (query_vector.empty()) {
        return kvai::infra::Status::InvalidArgument("query vector cannot be empty");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<kvai::core::SearchReference> matches;

    for (const auto& [_, entry] : entries_) {
        if (entry.record.collection != collection || !MatchesFilters(entry, filters)) {
            continue;
        }

        matches.push_back(kvai::core::SearchReference{entry.record.collection, entry.record.key, CosineSimilarity(query_vector, entry.vector), entry.record.metadata});
    }

    std::sort(matches.begin(), matches.end(), [](const auto& lhs, const auto& rhs) { return lhs.score > rhs.score; });
    if (matches.size() > top_k) {
        matches.resize(top_k);
    }

    return matches;
}

kvai::infra::Status PersistentVectorIndex::FlushSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = EnsureParentDirectory(snapshot_path_);
    if (!status.ok()) {
        return status;
    }

    std::ofstream output(snapshot_path_, std::ios::trunc);
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open index snapshot file");
    }

    for (const auto& [_, entry] : entries_) {
        output << Escape(entry.record.collection) << '|' << Escape(entry.record.key) << '|' << Escape(entry.record.title) << '|' << Escape(entry.record.body)
               << '|' << SerializeMetadata(entry.record.metadata) << '|' << SerializeVector(entry.vector) << '\n';
    }

    return kvai::infra::Status::Ok();
}

bool PersistentVectorIndex::MatchesFilters(const Entry& entry, const std::vector<SearchFilter>& filters) {
    for (const auto& filter : filters) {
        const auto iterator = entry.record.metadata.find(filter.field);
        if (iterator == entry.record.metadata.end() || iterator->second != filter.value) {
            return false;
        }
    }

    return true;
}

double PersistentVectorIndex::CosineSimilarity(const std::vector<float>& lhs, const std::vector<float>& rhs) {
    if (lhs.size() != rhs.size()) {
        return -1.0;
    }

    double dot = 0.0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        dot += static_cast<double>(lhs[index]) * static_cast<double>(rhs[index]);
    }
    return dot;
}

kvai::infra::StatusOr<std::unique_ptr<VectorIndex>> CreateVectorIndex(const kvai::infra::ServerConfig& config) {
#if defined(KVAI_HAVE_FAISS)
    if (config.search_backend == "faiss" || config.search_backend == "auto") {
        auto index = std::make_unique<FaissVectorIndex>(config.index_path, config.search_faiss_nlist, config.search_faiss_nprobe);
        auto status = index->Recover();
        if (!status.ok()) {
            if (config.search_backend == "faiss") {
                return status;
            }
            // auto mode: fall through to brute_force
        } else {
            return std::unique_ptr<VectorIndex>(std::move(index));
        }
    }
#else
    if (config.search_backend == "faiss") {
        return kvai::infra::Status::NotSupported("faiss backend requested but library is not available in this build");
    }
#endif

    if (config.search_backend != "brute_force" && config.search_backend != "auto") {
        return kvai::infra::Status::InvalidArgument("unsupported search backend: " + config.search_backend);
    }

    auto index = std::make_unique<PersistentVectorIndex>(config.index_path);
    auto status = index->Recover();
    if (!status.ok()) {
        return status;
    }
    return std::unique_ptr<VectorIndex>(std::move(index));
}

}  // namespace kvai::search