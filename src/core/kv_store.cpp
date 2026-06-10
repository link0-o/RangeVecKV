#include "core/kv_store.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>

#include "core/persistence_codec.h"

#if defined(KVAI_HAVE_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#endif

namespace kvai::core {

namespace {

std::string Unescape(const std::string& value) {
    std::string result;
    result.reserve(value.size());

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

std::map<std::string, std::string> DeserializeMetadata(const std::string& encoded) {
    std::map<std::string, std::string> metadata;
    if (encoded.empty()) {
        return metadata;
    }

    for (const auto& part : SplitEscaped(encoded, ';')) {
        const auto equal_pos = part.find('=');
        if (equal_pos == std::string::npos) {
            continue;
        }
        metadata.emplace(Unescape(part.substr(0, equal_pos)), Unescape(part.substr(equal_pos + 1)));
    }

    return metadata;
}

kvai::infra::StatusOr<DocumentRecord> DeserializeRecord(const std::string& encoded) {
    const auto parts = SplitEscaped(encoded, '|');
    if (parts.size() != 5) {
        return kvai::infra::Status::Internal("malformed record serialization");
    }

    DocumentRecord record;
    record.collection = Unescape(parts[0]);
    record.key = Unescape(parts[1]);
    record.title = Unescape(parts[2]);
    record.body = Unescape(parts[3]);
    record.metadata = DeserializeMetadata(parts[4]);
    return record;
}

[[maybe_unused]] kvai::infra::StatusOr<DocumentRecord> DecodeStoredRecord(const std::string& encoded) {
    auto decoded = persistence::DecodeDocumentRecord(encoded);
    if (decoded.ok() && !decoded.value().collection.empty() && !decoded.value().key.empty()) {
        return decoded;
    }
    return DeserializeRecord(encoded);
}

kvai::infra::StatusOr<std::string> ReadWholeFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return kvai::infra::Status::NotFound("file not found: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool HasPersistenceMagic(const std::string& bytes) {
    return bytes.rfind(persistence::MagicHeader(), 0) == 0;
}

bool ShouldRewriteLegacyWal(const std::string& path) {
    if (!std::filesystem::exists(path) || std::filesystem::file_size(path) == 0) {
        return false;
    }
    auto bytes = ReadWholeFile(path);
    return bytes.ok() && !HasPersistenceMagic(bytes.value());
}

kvai::infra::Status EnsureParentDirectory(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        return kvai::infra::Status::Ok();
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        return kvai::infra::Status::Internal("failed to create parent directory: " + error.message());
    }

    return kvai::infra::Status::Ok();
}

}  // namespace

#if defined(KVAI_HAVE_ROCKSDB)
class RocksDbKvStore final : public KvStore {
public:
    explicit RocksDbKvStore(std::string db_path) : db_path_(std::move(db_path)) {}

    ~RocksDbKvStore() override {
        if (db_ == nullptr) {
            return;
        }
        for (const auto& [name, handle] : column_families_) {
            (void)name;
            db_->DestroyColumnFamilyHandle(handle);
        }
    }

    kvai::infra::Status Recover() override {
        const auto directory_status = EnsureParentDirectory(db_path_);
        if (!directory_status.ok()) {
            return directory_status;
        }

        rocksdb::Options options;
        options.create_if_missing = true;
        options.create_missing_column_families = true;

        std::vector<std::string> names;
        const auto list_status = rocksdb::DB::ListColumnFamilies(options, db_path_, &names);
        if (!list_status.ok()) {
            names = {rocksdb::kDefaultColumnFamilyName};
        }

        std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
        descriptors.reserve(names.size());
        for (const auto& name : names) {
            descriptors.emplace_back(name, rocksdb::ColumnFamilyOptions());
        }

        std::vector<rocksdb::ColumnFamilyHandle*> handles;
        std::unique_ptr<rocksdb::DB> db;
        const auto status = rocksdb::DB::Open(options, db_path_, descriptors, &handles, &db);
        if (!status.ok()) {
            return kvai::infra::Status::Internal("failed to open rocksdb: " + status.ToString());
        }

        db_ = std::move(db);
        for (std::size_t index = 0; index < names.size(); ++index) {
            column_families_[names[index]] = handles[index];
        }
        return kvai::infra::Status::Ok();
    }

    kvai::infra::Status Put(const DocumentRecord& record) override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        auto* cf = GetOrCreateColumnFamily(record.collection);
        if (cf == nullptr) {
            return kvai::infra::Status::Internal("failed to get column family for collection: " + record.collection);
        }
        const auto status = db_->Put(rocksdb::WriteOptions(), cf, record.key, persistence::EncodeDocumentRecord(record));
        return status.ok() ? kvai::infra::Status::Ok() : kvai::infra::Status::Internal("rocksdb put failed: " + status.ToString());
    }

    kvai::infra::Status BatchPut(const std::vector<DocumentRecord>& records) override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        rocksdb::WriteBatch batch;
        for (const auto& record : records) {
            auto* cf = GetOrCreateColumnFamily(record.collection);
            if (cf == nullptr) {
                return kvai::infra::Status::Internal("failed to get column family for collection: " + record.collection);
            }
            batch.Put(cf, record.key, persistence::EncodeDocumentRecord(record));
        }
        const auto status = db_->Write(rocksdb::WriteOptions(), &batch);
        return status.ok() ? kvai::infra::Status::Ok() : kvai::infra::Status::Internal("rocksdb batch put failed: " + status.ToString());
    }

    kvai::infra::Status Delete(const std::string& collection, const std::string& key) override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        auto* cf = GetOrCreateColumnFamily(collection);
        if (cf == nullptr) {
            return kvai::infra::Status::Ok();  // Collection doesn't exist, nothing to delete
        }
        const auto status = db_->Delete(rocksdb::WriteOptions(), cf, key);
        return status.ok() ? kvai::infra::Status::Ok() : kvai::infra::Status::Internal("rocksdb delete failed: " + status.ToString());
    }

    kvai::infra::StatusOr<DocumentRecord> Get(const std::string& collection, const std::string& key) const override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        auto* cf = GetColumnFamily(collection);
        if (cf == nullptr) {
            return kvai::infra::Status::NotFound("document not found (collection does not exist)");
        }
        std::string value;
        const auto status = db_->Get(rocksdb::ReadOptions(), cf, key, &value);
        if (status.IsNotFound()) {
            return kvai::infra::Status::NotFound("document not found");
        }
        if (!status.ok()) {
            return kvai::infra::Status::Internal("rocksdb get failed: " + status.ToString());
        }
        return DecodeStoredRecord(value);
    }

    kvai::infra::StatusOr<std::vector<DocumentRecord>> MultiGet(const std::vector<SearchReference>& references) const override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        if (references.empty()) {
            return std::vector<DocumentRecord>{};
        }

        // Group references by collection for batched MultiGet
        std::map<std::string, std::vector<std::size_t>> collection_groups;
        for (std::size_t index = 0; index < references.size(); ++index) {
            collection_groups[references[index].collection].push_back(index);
        }

        std::vector<DocumentRecord> results;
        results.reserve(references.size());

        for (const auto& [collection, indices] : collection_groups) {
            auto* cf = GetColumnFamily(collection);
            if (cf == nullptr) {
                continue;  // Collection doesn't exist
            }

            // Build keys and handles for RocksDB MultiGet
            std::vector<rocksdb::Slice> keys;
            keys.reserve(indices.size());

            for (const auto index : indices) {
                keys.emplace_back(references[index].key);
            }

            std::vector<rocksdb::ColumnFamilyHandle*> handles(keys.size(), cf);
            std::vector<std::string> values;
            const auto statuses = db_->MultiGet(rocksdb::ReadOptions(), handles, keys, &values);

            for (std::size_t index = 0; index < indices.size(); ++index) {
                if (statuses[index].ok()) {
                    auto record = DecodeStoredRecord(values[index]);
                    if (record.ok()) {
                        results.push_back(record.value());
                    }
                }
            }
        }

        return results;
    }

    kvai::infra::StatusOr<std::vector<DocumentRecord>> Range(const std::string& collection,
                                                             const std::string& begin_key,
                                                             const std::string& end_key,
                                                             std::size_t limit) const override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        auto* cf = GetColumnFamily(collection);
        if (cf == nullptr) {
            return std::vector<DocumentRecord>{};  // Collection doesn't exist
        }

        std::vector<DocumentRecord> results;
        std::unique_ptr<rocksdb::Iterator> iterator(db_->NewIterator(rocksdb::ReadOptions(), cf));

        if (begin_key.empty()) {
            iterator->SeekToFirst();
        } else {
            iterator->Seek(begin_key);
        }

        for (; iterator->Valid(); iterator->Next()) {
            if (!end_key.empty() && iterator->key().ToString() >= end_key) {
                break;
            }
            auto record = DecodeStoredRecord(iterator->value().ToString());
            if (!record.ok()) {
                return record.status();
            }
            results.push_back(record.value());
            if (results.size() >= limit) {
                break;
            }
        }
        return results;
    }

    kvai::infra::StatusOr<std::vector<std::string>> Collections() const override {
        std::lock_guard<std::mutex> lock(cf_mutex_);
        std::vector<std::string> collections;
        collections.reserve(column_families_.size());
        for (const auto& [name, _] : column_families_) {
            (void)_;
            collections.push_back(name);
        }
        return collections;
    }

    kvai::infra::Status FlushSnapshot() override {
        if (db_ != nullptr) {
            rocksdb::FlushOptions flush_options;
            flush_options.wait = true;
            db_->Flush(flush_options);
        }
        return kvai::infra::Status::Ok();
    }

private:
    rocksdb::ColumnFamilyHandle* GetOrCreateColumnFamily(const std::string& collection) {
        std::lock_guard<std::mutex> lock(cf_mutex_);
        auto iterator = column_families_.find(collection);
        if (iterator != column_families_.end()) {
            return iterator->second;
        }

        rocksdb::ColumnFamilyHandle* cf = nullptr;
        auto status = db_->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), collection, &cf);
        if (!status.ok()) {
            return nullptr;
        }

        column_families_[collection] = cf;
        return cf;
    }

    rocksdb::ColumnFamilyHandle* GetColumnFamily(const std::string& collection) const {
        std::lock_guard<std::mutex> lock(cf_mutex_);
        auto iterator = column_families_.find(collection);
        if (iterator != column_families_.end()) {
            return iterator->second;
        }
        return nullptr;
    }

    std::string db_path_;
    std::unique_ptr<rocksdb::DB> db_;
    mutable std::mutex cf_mutex_;
    std::map<std::string, rocksdb::ColumnFamilyHandle*> column_families_;
};
#endif

WriteAheadKvStore::WriteAheadKvStore(std::string wal_path, std::string snapshot_path)
    : wal_path_(std::move(wal_path)), snapshot_path_(std::move(snapshot_path)) {}

WriteAheadKvStore::~WriteAheadKvStore() {
    (void)FlushSnapshot();
}

kvai::infra::Status WriteAheadKvStore::Recover() {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();

    auto status = ReplaySnapshotLocked();
    if (!status.ok()) {
        return status;
    }

    return ReplayWalLocked();
}

kvai::infra::Status WriteAheadKvStore::Put(const DocumentRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_[BuildCompositeKey(record.collection, record.key)] = record;
    return AppendWalLocked('P', record);
}

kvai::infra::Status WriteAheadKvStore::BatchPut(const std::vector<DocumentRecord>& records) {
    for (const auto& record : records) {
        const auto status = Put(record);
        if (!status.ok()) {
            return status;
        }
    }

    return kvai::infra::Status::Ok();
}

kvai::infra::Status WriteAheadKvStore::Delete(const std::string& collection, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.erase(BuildCompositeKey(collection, key));
    return AppendDeleteLocked(collection, key);
}

kvai::infra::StatusOr<DocumentRecord> WriteAheadKvStore::Get(const std::string& collection, const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = records_.find(BuildCompositeKey(collection, key));
    if (iterator == records_.end()) {
        return kvai::infra::Status::NotFound("document not found");
    }
    return iterator->second;
}

kvai::infra::StatusOr<std::vector<DocumentRecord>> WriteAheadKvStore::MultiGet(const std::vector<SearchReference>& references) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DocumentRecord> results;
    results.reserve(references.size());

    for (const auto& reference : references) {
        const auto iterator = records_.find(BuildCompositeKey(reference.collection, reference.key));
        if (iterator != records_.end()) {
            results.push_back(iterator->second);
        }
    }

    return results;
}

kvai::infra::StatusOr<std::vector<DocumentRecord>> WriteAheadKvStore::Range(const std::string& collection,
                                                                             const std::string& begin_key,
                                                                             const std::string& end_key,
                                                                             std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DocumentRecord> results;
    const auto start = BuildCompositeKey(collection, begin_key);
    const auto finish = end_key.empty() ? BuildCompositeKey(collection, "\x7f") : BuildCompositeKey(collection, end_key);

    for (auto iterator = records_.lower_bound(start); iterator != records_.end(); ++iterator) {
        if (iterator->first > finish || iterator->second.collection != collection) {
            break;
        }

        results.push_back(iterator->second);
        if (results.size() >= limit) {
            break;
        }
    }

    return results;
}

kvai::infra::StatusOr<std::vector<std::string>> WriteAheadKvStore::Collections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> names;
    for (const auto& [_, record] : records_) {
        (void)_;
        if (!record.collection.empty()) {
            names.insert(record.collection);
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

kvai::infra::Status WriteAheadKvStore::FlushSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = EnsureParentDirectory(snapshot_path_);
    if (!status.ok()) {
        return status;
    }

    std::ofstream output(snapshot_path_, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open snapshot file for writing");
    }

    output << persistence::MagicHeader();
    for (const auto& [_, record] : records_) {
        output << persistence::EncodeFrame(persistence::EncodeDocumentRecord(record));
    }

    return kvai::infra::Status::Ok();
}

kvai::infra::Status WriteAheadKvStore::ReplaySnapshotLocked() {
    auto snapshot = ReadWholeFile(snapshot_path_);
    if (!snapshot.ok()) {
        return kvai::infra::Status::Ok();
    }

    if (HasPersistenceMagic(snapshot.value())) {
        auto frames = persistence::DecodeFrames(snapshot.value());
        if (!frames.ok()) {
            return frames.status();
        }
        for (const auto& frame : frames.value()) {
            auto record = persistence::DecodeDocumentRecord(frame);
            if (!record.ok()) {
                return record.status();
            }
            records_[BuildCompositeKey(record.value().collection, record.value().key)] = record.value();
        }
        return kvai::infra::Status::Ok();
    }

    std::istringstream input(snapshot.value());
    std::string line;
    while (std::getline(input, line)) {
        auto record = DeserializeRecord(line);
        if (!record.ok()) {
            return record.status();
        }
        records_[BuildCompositeKey(record.value().collection, record.value().key)] = record.value();
    }

    return kvai::infra::Status::Ok();
}

kvai::infra::Status WriteAheadKvStore::ReplayWalLocked() {
    auto wal = ReadWholeFile(wal_path_);
    if (!wal.ok()) {
        return kvai::infra::Status::Ok();
    }

    if (HasPersistenceMagic(wal.value())) {
        auto frames = persistence::DecodeFrames(wal.value());
        if (!frames.ok()) {
            return frames.status();
        }
        for (const auto& frame : frames.value()) {
            auto entry = persistence::DecodeWalEntry(frame);
            if (!entry.ok()) {
                return entry.status();
            }
            if (entry.value().operation == persistence::WalOperation::kPut) {
                records_[BuildCompositeKey(entry.value().record.collection, entry.value().record.key)] = entry.value().record;
            } else {
                records_.erase(BuildCompositeKey(entry.value().collection, entry.value().key));
            }
        }
        return kvai::infra::Status::Ok();
    }

    std::istringstream input(wal.value());
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 2 || line[1] != '|') {
            continue;
        }

        const auto operation = line[0];
        if (operation == 'P') {
            auto record = DeserializeRecord(line.substr(2));
            if (!record.ok()) {
                return record.status();
            }
            records_[BuildCompositeKey(record.value().collection, record.value().key)] = record.value();
        } else if (operation == 'D') {
            const auto parts = SplitEscaped(line.substr(2), '|');
            if (parts.size() >= 2) {
                records_.erase(BuildCompositeKey(Unescape(parts[0]), Unescape(parts[1])));
            }
        }
    }

    return kvai::infra::Status::Ok();
}

kvai::infra::Status WriteAheadKvStore::AppendWalLocked(char operation, const DocumentRecord& record) {
    auto status = EnsureParentDirectory(wal_path_);
    if (!status.ok()) {
        return status;
    }

    const bool rewrite_legacy = ShouldRewriteLegacyWal(wal_path_);
    const bool needs_header = rewrite_legacy || !std::filesystem::exists(wal_path_) || std::filesystem::file_size(wal_path_) == 0;
    std::ofstream output(wal_path_, std::ios::binary | (rewrite_legacy ? std::ios::trunc : std::ios::app));
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open wal file for writing");
    }

    if (needs_header) {
        output << persistence::MagicHeader();
    }
    if (rewrite_legacy) {
        for (const auto& [_, current] : records_) {
            (void)_;
            persistence::WalEntry current_entry;
            current_entry.operation = persistence::WalOperation::kPut;
            current_entry.record = current;
            output << persistence::EncodeFrame(persistence::EncodeWalEntry(current_entry));
        }
        return kvai::infra::Status::Ok();
    }
    persistence::WalEntry entry;
    entry.operation = operation == 'D' ? persistence::WalOperation::kDelete : persistence::WalOperation::kPut;
    entry.record = record;
    output << persistence::EncodeFrame(persistence::EncodeWalEntry(entry));
    return kvai::infra::Status::Ok();
}

kvai::infra::Status WriteAheadKvStore::AppendDeleteLocked(const std::string& collection, const std::string& key) {
    auto status = EnsureParentDirectory(wal_path_);
    if (!status.ok()) {
        return status;
    }

    const bool rewrite_legacy = ShouldRewriteLegacyWal(wal_path_);
    const bool needs_header = rewrite_legacy || !std::filesystem::exists(wal_path_) || std::filesystem::file_size(wal_path_) == 0;
    std::ofstream output(wal_path_, std::ios::binary | (rewrite_legacy ? std::ios::trunc : std::ios::app));
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open wal file for writing");
    }

    if (needs_header) {
        output << persistence::MagicHeader();
    }
    if (rewrite_legacy) {
        for (const auto& [_, current] : records_) {
            (void)_;
            persistence::WalEntry current_entry;
            current_entry.operation = persistence::WalOperation::kPut;
            current_entry.record = current;
            output << persistence::EncodeFrame(persistence::EncodeWalEntry(current_entry));
        }
        return kvai::infra::Status::Ok();
    }
    persistence::WalEntry entry;
    entry.operation = persistence::WalOperation::kDelete;
    entry.collection = collection;
    entry.key = key;
    output << persistence::EncodeFrame(persistence::EncodeWalEntry(entry));
    return kvai::infra::Status::Ok();
}

kvai::infra::StatusOr<std::unique_ptr<KvStore>> CreateKvStore(const kvai::infra::ServerConfig& config) {
#if defined(KVAI_HAVE_ROCKSDB)
    if (config.storage_backend == "rocksdb" || config.storage_backend == "auto") {
        auto store = std::make_unique<RocksDbKvStore>(config.db_path);
        auto status = store->Recover();
        if (!status.ok()) {
            if (config.storage_backend == "rocksdb") {
                return status;
            }
            // auto mode: fall through to wal
        } else {
            return std::unique_ptr<KvStore>(std::move(store));
        }
    }
#else
    if (config.storage_backend == "rocksdb") {
        return kvai::infra::Status::NotSupported("rocksdb backend requested but library is not available in this build");
    }
#endif

    if (config.storage_backend != "wal" && config.storage_backend != "auto") {
        return kvai::infra::Status::InvalidArgument("unsupported storage backend: " + config.storage_backend);
    }

    auto store = std::make_unique<WriteAheadKvStore>(config.wal_path, config.snapshot_path);
    auto status = store->Recover();
    if (!status.ok()) {
        return status;
    }
    return std::unique_ptr<KvStore>(std::move(store));
}

}  // namespace kvai::core
