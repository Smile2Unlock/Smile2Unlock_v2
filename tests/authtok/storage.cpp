#include <sys/stat.h>
#include <unistd.h>

import std;
import su.auth.storage;
import su.auth.user;
import su.core.types;

namespace {
int failures = 0;
void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}
auto key(const std::filesystem::path& path, su::auth::KeyProtection protection) {
    auto bytes = std::array<unsigned char, 44>{};
    bytes[0] = 'S';
    bytes[1] = '2';
    bytes[2] = 'U';
    bytes[3] = 'K';
    bytes[5] = 1;
    bytes[6] = static_cast<unsigned char>(protection);
    bytes[11] = 1;
    std::fill(bytes.begin() + 12, bytes.end(), 0x42);
    {
        auto out = std::ofstream(path, std::ios::binary);
        out.write(reinterpret_cast<char*>(bytes.data()), bytes.size());
    }
    ::chmod(path.c_str(), 0600);
    return su::auth::load_key_credential(path);
}
} // namespace

int main() {
    using namespace su::auth;
    using Type = su::app::ControlMessageType;
    const auto root =
        std::filesystem::absolute("build/test-data") / std::format("authtok-{}", ::getpid());
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto tpm = key(root / "tpm.key", KeyProtection::kTpm2Bound);
    auto host = key(root / "host.key", KeyProtection::kHostKey);
    check(tpm && host, "test master keys load");
    if (!tpm || !host)
        return 1;
    const auto path = (root / "1000/authtok.s2u").string();
    check(!enroll_authtok(nullptr, 1000, path, "wallet-token"),
          "missing protection refuses enrollment");
    check(!enroll_authtok(&*host, 1000, path, "wallet-token"),
          "host-only protection refuses enrollment");
    check(!std::filesystem::exists(path), "refused enrollment writes nothing");
    check(enroll_authtok(&*tpm, 1000, path, "wallet-token").has_value(),
          "host+tpm2 enrollment succeeds");
    check(su::app::authtok_enrolled(path).value_or(false), "status exposes enrollment boolean");
    const auto allowed = authenticate_authtok(&*tpm, 1000, path, 0, true, true);
    check(allowed && *allowed == R"({"authtok":"d2FsbGV0LXRva2Vu"})",
          "accepted same-user root request receives base64");
    for (const auto peer : {1000u, 1001u}) {
        const auto blocked = authenticate_authtok(&*tpm, 1000, path, peer, true, true);
        check(blocked && blocked->empty(), "non-root want_authtok never receives secret");
    }
    check(authenticate_authtok(&*tpm, 1000, path, 0, true, false)->empty(),
          "rejected auth never releases secret");
    check(authenticate_authtok(&*tpm, 1000, path, 0, false, true)->empty(),
          "default-off request never releases secret");
    check(!authenticate_authtok(&*tpm, 1001, path, 0, true, true),
          "UID-bound ciphertext cannot unlock another account");
    check(!authenticate_authtok(&*host, 1000, path, 0, true, true),
          "host-only key never releases sealed token");
    auto in = std::ifstream(path, std::ios::binary);
    const auto ciphertext = std::string(std::istreambuf_iterator<char>(in), {});
    check(ciphertext.find("wallet-token") == std::string::npos,
          "secret absent from stored ciphertext");
    for (const auto type : {Type::kSetAuthtok, Type::kClearAuthtok}) {
        check(peer_request_allowed(1000, type, 1000), "own-user management allowed");
        check(!peer_request_allowed(1000, type, 1001), "foreign-user set/clear rejected");
        check(peer_request_allowed(0, type, 1001), "root may target other users");
        const auto operation = management_operation_for_message(type);
        check(operation.has_value(), "token management requires capability");
        auto capabilities = ManagementCapabilityStore{};
        check(!capabilities.consume("", 1000, 10, 10, *operation), "missing capability rejected");
        auto token = capabilities.issue(1000, 10, 10, *operation);
        check(token && capabilities.consume(*token, 1000, 10, 10, *operation),
              "matching capability consumed");
        check(!capabilities.consume(*token, 1000, 10, 10, *operation),
              "capability cannot be reused");
    }
    check(su::app::clear_authtok(path).value_or(false), "clear removes record");
    check(!su::app::authtok_enrolled(path).value_or(true), "cleared record absent from status");
    check(!authenticate_authtok(&*tpm, 1000, path, 0, true, true),
          "cleared record cannot be released");
    std::filesystem::remove_all(root);
    if (!failures)
        std::cout << "authtok daemon storage and policy checks passed\n";
    return failures ? 1 : 0;
}
