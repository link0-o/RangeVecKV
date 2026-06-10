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

    auto range = recovered.Range("documents", "", "", 10);
    if (!Expect(range.ok(), "range failed")) {
        return 1;
    }
    if (!Expect(range.value().size() == 1, "range size mismatch")) {
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
