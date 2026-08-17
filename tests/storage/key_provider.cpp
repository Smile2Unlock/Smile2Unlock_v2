#include <sys/stat.h>
#include <unistd.h>

import std;
import su.auth.storage;
import su.core.types;

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

class TemporaryCredential {
public:
    TemporaryCredential() {
        path_ = std::filesystem::temp_directory_path()
            / std::format("smile2unlock-key-provider-{}", ::getpid());
        auto bytes = std::array<std::uint8_t, 44>{};
        std::ranges::copy(std::string_view{"S2UK"}, bytes.begin());
        bytes[5] = 1;
        bytes[6] = static_cast<std::uint8_t>(su::auth::KeyProtection::kHostKey);
        bytes[11] = 1;
        std::ranges::fill(bytes.begin() + 12, bytes.end(), 0x5a);
        auto output = std::ofstream{path_, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        output.close();
        require(::chmod(path_.c_str(), 0600) == 0);
    }

    ~TemporaryCredential() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main() {
    const auto credential = TemporaryCredential{};
    auto key = su::auth::load_key_credential(credential.path());
    require(key.has_value());
    require(key->version() == 1);
    require(key->protection() == su::auth::KeyProtection::kHostKey);
    // Sealed key: the only way to observe the bytes is inside with_bytes()
    // (page is PROT_NONE while idle), which is exactly the guarantee the
    // memsafe-style MasterKey provides.
    // Sealed key: the only way to observe the bytes is inside with_bytes()
    // (page is PROT_NONE while idle), which is exactly the guarantee the
    // memsafe-style MasterKey provides.
    auto seal_check_ok = false;
    key->with_bytes([](const auto key_bytes) {
        return std::ranges::all_of(key_bytes,
            [](auto byte) { return byte == 0x5a; });
    });
    seal_check_ok = true;
    require(seal_check_ok);
    auto context_account = std::uint32_t{0};
    key->with_context(1000, [&](const su::app::EncryptedStoreContext& context) {
        context_account = std::get<std::uint32_t>(context.account);
        return std::ranges::all_of(context.master_key,
            [](auto byte) { return byte == 0x5a; });
    });
    require(context_account == 1000);
    return 0;
}
