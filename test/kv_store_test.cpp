#include <filesystem>
#include <fstream>
#include <iostream>

#include "core/kv_store.h"
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
    const auto temp_dir = std::filesystem::temp_directory_path() / "rangeveckv-kv-test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    kvai::core::WriteAheadKvStore store((temp_dir / "store.wal").string(), (temp_dir / "store.snapshot").string());

    kvai::core::DocumentRecord record{"documents", "doc-1", "Test title", "Test body", {{"team", "search"}}};
    record.version = 10;
    record.updated_at_unix_ms = 10;
    record.mutation_id = "mutation-10";

    if (!Expect(store.Put(record).ok(), "put failed")) {
        return 1;
    }
    if (!Expect(store.FlushSnapshot().ok(), "snapshot failed")) {
        return 1;
    }

    kvai::core::WriteAheadKvStore recovered((temp_dir / "store.wal").string(), (temp_dir / "store.snapshot").string());
    if (!Expect(recovered.Recover().ok(), "recover failed")) {
        return 1;
    }

    auto loaded = recovered.Get("documents", "doc-1");
    if (!Expect(loaded.ok(), "loaded document missing")) {
        return 1;
    }
    if (!Expect(loaded.value().title == "Test title", "loaded title mismatch")) {
        return 1;
    }
    if (!Expect(loaded.value().version == 10 && loaded.value().mutation_id == "mutation-10", "loaded mutation metadata mismatch")) {
        return 1;
    }
    kvai::core::DocumentRecord stale = record;
    stale.body = "stale";
    stale.version = 9;
    stale.mutation_id = "mutation-9";
    if (!Expect(!recovered.Put(stale).ok(), "stale mutation should be rejected")) {
        return 1;
    }
    kvai::core::DocumentRecord duplicate = record;
    duplicate.body = "duplicate retry";
    if (!Expect(recovered.Put(duplicate).ok(), "duplicate mutation should be idempotent")) {
        return 1;
    }
    auto after_duplicate = recovered.Get("documents", "doc-1");
    if (!Expect(after_duplicate.ok() && after_duplicate.value().body == "Test body", "duplicate mutation should not overwrite")) {
        return 1;
    }

    kvai::core::DocumentRecord batch_ok_a{"documents", "batch-1", "Batch 1", "Batch body 1", {}};
    batch_ok_a.version = 20;
    batch_ok_a.updated_at_unix_ms = 20;
    batch_ok_a.mutation_id = "batch-20-a";
    kvai::core::DocumentRecord batch_ok_b{"documents", "batch-2", "Batch 2", "Batch body 2", {}};
    batch_ok_b.version = 21;
    batch_ok_b.updated_at_unix_ms = 21;
    batch_ok_b.mutation_id = "batch-21-b";
    if (!Expect(recovered.BatchPut({batch_ok_a, batch_ok_b}).ok(), "batch put failed")) {
        return 1;
    }
    if (!Expect(recovered.Get("documents", "batch-1").ok() && recovered.Get("documents", "batch-2").ok(),
                "batch put records missing")) {
        return 1;
    }
    kvai::core::DocumentRecord duplicate_key = batch_ok_a;
    duplicate_key.mutation_id = "batch-duplicate-key";
    if (!Expect(!recovered.BatchPut({batch_ok_a, duplicate_key}).ok(), "batch duplicate key should be rejected")) {
        return 1;
    }
    kvai::core::DocumentRecord batch_partial{"documents", "batch-partial", "Partial", "should not write", {}};
    batch_partial.version = 30;
    batch_partial.updated_at_unix_ms = 30;
    batch_partial.mutation_id = "batch-partial-30";
    kvai::core::DocumentRecord batch_stale = record;
    batch_stale.body = "stale in batch";
    batch_stale.version = 9;
    batch_stale.mutation_id = "batch-stale-9";
    if (!Expect(!recovered.BatchPut({batch_partial, batch_stale}).ok(), "batch with stale mutation should fail")) {
        return 1;
    }
    if (!Expect(!recovered.Get("documents", "batch-partial").ok(), "failed batch should not partially write records")) {
        return 1;
    }

    auto range = recovered.Range("documents", "", "", 10);
    if (!Expect(range.ok(), "range failed")) {
        return 1;
    }
    if (!Expect(range.value().size() == 3, "range size mismatch")) {
        return 1;
    }
    auto collections = recovered.Collections();
    if (!Expect(collections.ok(), "collections failed")) {
        return 1;
    }
    if (!Expect(!collections.value().empty() && collections.value().front() == "documents", "collections missing documents")) {
        return 1;
    }

    const auto legacy_wal = temp_dir / "legacy.wal";
    const auto legacy_snapshot = temp_dir / "legacy.snapshot";
    {
        std::ofstream output(legacy_wal);
        output << "P|documents|legacy-1|Legacy title|Legacy body|team=search\n";
    }
    kvai::core::WriteAheadKvStore legacy_store(legacy_wal.string(), legacy_snapshot.string());
    if (!Expect(legacy_store.Recover().ok(), "legacy recover failed")) {
        return 1;
    }
    auto legacy_loaded = legacy_store.Get("documents", "legacy-1");
    if (!Expect(legacy_loaded.ok(), "legacy document missing")) {
        return 1;
    }
    kvai::core::DocumentRecord migrated{"documents", "legacy-2", "Migrated", "protobuf wal", {}};
    if (!Expect(legacy_store.Put(migrated).ok(), "legacy wal migration put failed")) {
        return 1;
    }
    {
        std::ifstream input(legacy_wal, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (!Expect(bytes.rfind(kvai::core::persistence::MagicHeader(), 0) == 0, "legacy wal was not rewritten as protobuf")) {
            return 1;
        }
    }

    std::filesystem::remove_all(temp_dir);
    return 0;
}
