#include <filesystem>
#include <iostream>

#include "core/kv_store.h"

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

    auto range = recovered.Range("documents", "", "", 10);
    if (!Expect(range.ok(), "range failed")) {
        return 1;
    }
    if (!Expect(range.value().size() == 1, "range size mismatch")) {
        return 1;
    }

    std::filesystem::remove_all(temp_dir);
    return 0;
}