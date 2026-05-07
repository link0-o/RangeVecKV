#include "core/document.h"

namespace kvai::core {

std::string BuildCompositeKey(const std::string& collection, const std::string& key) {
    return collection + "\x1f" + key;
}

}  // namespace kvai::core