#include "core/kv_store.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(KVAI_HAVE_ROCKSDB)
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#endif

namespace kvai::core {

namespace {

std::string Escape(std::string value) {
    std::string escaped;
    escaped.reserve(value.size());

    for (char ch : value) {
        switch (ch) {
        case '\\':
        case '|':
        case ';':
        case '=':
        case '\n':
            escaped.push_back('\\');
            if (ch == '\n') {
                escaped.push_back('n');
            } else {
                escaped.push_back(ch);
            }
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }

    return escaped;
}

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

std::string SerializeRecord(const DocumentRecord& record) {
    std::ostringstream stream;
    stream << Escape(record.collection) << '|' << Escape(record.key) << '|' << Escape(record.title) << '|' << Escape(record.body) << '|'
           << SerializeMetadata(record.metadata);
    return stream.str();
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

    kvai::infra::Status Recover() override {
        rocksdb::Options options;
        options.create_if_missing = true;
        rocksdb::DB* raw_db = nullptr;
        const auto status = rocksdb::DB::Open(options, db_path_, &raw_db);
        if (!status.ok()) {
            return kvai::infra::Status::Internal("failed to open rocksdb: " + status.ToString());
        }
        db_.reset(raw_db);
        return kvai::infra::Status::Ok();
    }

    kvai::infra::Status Put(const DocumentRecord& record) override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        const auto status = db_->Put(rocksdb::WriteOptions(), BuildCompositeKey(record.collection, record.key), SerializeRecord(record));
        return status.ok() ? kvai::infra::Status::Ok() : kvai::infra::Status::Internal("rocksdb put failed: " + status.ToString());
    }

    kvai::infra::Status BatchPut(const std::vector<DocumentRecord>& records) override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        rocksdb::WriteBatch batch;
        for (const auto& record : records) {
            batch.Put(BuildCompositeKey(record.collection, record.key), SerializeRecord(record));
        }
        const auto status = db_->Write(rocksdb::WriteOptions(), &batch);
        return status.ok() ? kvai::infra::Status::Ok() : kvai::infra::Status::Internal("rocksdb batch put failed: " + status.ToString());
    }

    kvai::infra::Status Delete(const std::string& collection, const std::string& key) override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        const auto status = db_->Delete(rocksdb::WriteOptions(), BuildCompositeKey(collection, key));
        return status.ok() ? kvai::infra::Status::Ok() : kvai::infra::Status::Internal("rocksdb delete failed: " + status.ToString());
    }

    kvai::infra::StatusOr<DocumentRecord> Get(const std::string& collection, const std::string& key) const override {
        if (db_ == nullptr) {
            return kvai::infra::Status::Unavailable("rocksdb store not initialized");
        }
        std::string value;
        const auto status = db_->Get(rocksdb::ReadOptions(), BuildCompositeKey(collection, key), &value);
        if (status.IsNotFound()) {
            return kvai::infra::Status::NotFound("document not found");
        }
        if (!status.ok()) {
            return kvai::infra::Status::Internal("rocksdb get failed: " + status.ToString());
        }
        return DeserializeRecord(value);
    }

    kvai::infra::StatusOr<std::vector<DocumentRecord>> MultiGet(const std::vector<SearchReference>& references) const override {
        std::vector<DocumentRecord> results;
        results.reserve(references.size());
        for (const auto& reference : references) {
            auto record = Get(reference.collection, reference.key);
            if (record.ok()) {
                results.push_back(record.value());
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
        std::vector<DocumentRecord> results;
        const auto start = BuildCompositeKey(collection, begin_key);
        const auto finish = end_key.empty() ? BuildCompositeKey(collection, "\x7f") : BuildCompositeKey(collection, end_key);

        std::unique_ptr<rocksdb::Iterator> iterator(db_->NewIterator(rocksdb::ReadOptions()));
        for (iterator->Seek(start); iterator->Valid(); iterator->Next()) {
            if (iterator->key().ToString() > finish) {
                break;
            }
            auto record = DeserializeRecord(iterator->value().ToString());
            if (!record.ok()) {
                return record.status();
            }
            if (record.value().collection != collection) {
                continue;
            }
            results.push_back(record.value());
            if (results.size() >= limit) {
                break;
            }
        }
        return results;
    }

    kvai::infra::Status FlushSnapshot() override {
        return kvai::infra::Status::Ok();
    }

private:
    struct RocksDbDeleter {
        void operator()(rocksdb::DB* db) const { delete db; }
    };

    std::string db_path_;
    std::unique_ptr<rocksdb::DB, RocksDbDeleter> db_;
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

kvai::infra::Status WriteAheadKvStore::FlushSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = EnsureParentDirectory(snapshot_path_);
    if (!status.ok()) {
        return status;
    }

    std::ofstream output(snapshot_path_, std::ios::trunc);
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open snapshot file for writing");
    }

    for (const auto& [_, record] : records_) {
        output << SerializeRecord(record) << '\n';
    }

    return kvai::infra::Status::Ok();
}

kvai::infra::Status WriteAheadKvStore::ReplaySnapshotLocked() {
    std::ifstream input(snapshot_path_);
    if (!input.is_open()) {
        return kvai::infra::Status::Ok();
    }

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
    std::ifstream input(wal_path_);
    if (!input.is_open()) {
        return kvai::infra::Status::Ok();
    }

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

    std::ofstream output(wal_path_, std::ios::app);
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open wal file for writing");
    }

    output << operation << '|' << SerializeRecord(record) << '\n';
    return kvai::infra::Status::Ok();
}

kvai::infra::Status WriteAheadKvStore::AppendDeleteLocked(const std::string& collection, const std::string& key) {
    auto status = EnsureParentDirectory(wal_path_);
    if (!status.ok()) {
        return status;
    }

    std::ofstream output(wal_path_, std::ios::app);
    if (!output.is_open()) {
        return kvai::infra::Status::Internal("failed to open wal file for writing");
    }

    output << 'D' << '|' << Escape(collection) << '|' << Escape(key) << '\n';
    return kvai::infra::Status::Ok();
}

kvai::infra::StatusOr<std::unique_ptr<KvStore>> CreateKvStore(const kvai::infra::ServerConfig& config) {
#if defined(KVAI_HAVE_ROCKSDB)
    if (config.storage_backend == "rocksdb") {
        auto store = std::make_unique<RocksDbKvStore>(config.db_path);
        auto status = store->Recover();
        if (!status.ok()) {
            return status;
        }
        return std::unique_ptr<KvStore>(std::move(store));
    }
#else
    if (config.storage_backend == "rocksdb") {
        return kvai::infra::Status::NotSupported("rocksdb backend requested but library is not available in this build");
    }
#endif

    if (config.storage_backend != "wal") {
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