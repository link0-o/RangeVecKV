#include <iostream>
#include <map>

#include "gateway/auth.h"

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
    kvai::gateway::ApiKeyAuthenticator auth("secret", true);

    if (!Expect(auth.Authenticate({{"x-api-key", "secret"}}).ok(), "x-api-key should authenticate")) ++failures;
    if (!Expect(auth.Authenticate({{"authorization", "Bearer secret"}}).ok(), "bearer should authenticate")) ++failures;
    if (!Expect(!auth.Authenticate({{"x-api-key", "wrong"}}).ok(), "wrong key should fail")) ++failures;
    if (!Expect(!auth.Authenticate({}).ok(), "missing key should fail")) ++failures;

    kvai::gateway::ApiKeyAuthenticator empty_required("", true);
    if (!Expect(!empty_required.Authenticate({{"x-api-key", ""}}).ok(), "empty configured key should fail closed")) ++failures;

    kvai::gateway::ApiKeyAuthenticator disabled("", false);
    if (!Expect(disabled.Authenticate({}).ok(), "disabled auth should pass")) ++failures;

    return failures == 0 ? 0 : 1;
}
