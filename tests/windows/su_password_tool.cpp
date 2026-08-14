// su_password_tool — diagnostic + enrollment helper for the Smile2Unlock
// logon-secret store. Runs on the target Windows VM as Administrator:
//   su_password_tool.exe keytest
//   su_password_tool.exe store <sid> <username> <password>
//   su_password_tool.exe prepare <sid> <request_id>
//   su_password_tool.exe stale <sid>
//   su_password_tool.exe clear <sid>
// ASCII-only input is expected (SIDs and test passwords).

#include "storage_key_provider.h"
#include "logon_secret_store.h"

#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using su::windows::security::LogonSecretError;
using su::windows::security::LogonSecretStore;
using su::windows::security::StorageKey;
using su::windows::security::StorageKeyError;

static std::wstring ascii_to_wide(const char* text) {
    std::wstring wide;
    while (*text) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
    }
    return wide;
}

static void usage() {
    std::cerr
        << "usage:\n"
        << "  su_password_tool keytest\n"
        << "  su_password_tool store <sid> <username> <password>\n"
        << "  su_password_tool prepare <sid> <request_id>\n"
        << "  su_password_tool stale <sid>\n"
        << "  su_password_tool clear <sid>\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const auto key_path = su::windows::security::default_storage_key_path();
    std::cout << "[tool] key_path=" << key_path.string() << "\n";
    auto key = su::windows::security::load_or_create_storage_key(key_path);
    if (!key) {
        std::cout << "[tool] storage key FAILED: "
                  << su::windows::security::storage_key_error_message(key.error()) << "\n";
        return 1;
    }
    std::cout << "[tool] storage key OK\n";

    const std::string command{argv[1]};
    if (command == "keytest") {
        std::cout << "[tool] keytest done\n";
        return 0;
    }

    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string sid{argv[2]};
    LogonSecretStore store(*key);

    if (command == "store") {
        if (argc != 5) {
            usage();
            return 2;
        }
        const std::string username{argv[3]};
        const auto password = ascii_to_wide(argv[4]);
        auto result = store.store(
            sid, username, SuWindowsAccountKind_Local,
            std::span<const wchar_t>{password});
        if (result) {
            std::cout << "[tool] store OK generation=" << *result << "\n";
            return 0;
        }
        std::cout << "[tool] store FAILED: "
                  << su::windows::security::logon_secret_error_message(result.error()) << "\n";
        return 1;
    }

    if (command == "prepare") {
        const unsigned long long request_id = std::strtoull(argv[3], nullptr, 10);
        auto result = store.prepare(sid, request_id, 0);
        if (result) {
            std::cout << "[tool] prepare OK utf16len=" << result->size() << "\n";
            // Do not print the secret itself; length + first-byte marker only.
            const auto* raw = result->c_str();
            if (result->size() > 0) {
                std::cout << "[tool] prepare first_char="
                          << static_cast<int>(static_cast<unsigned char>(raw[0])) << "\n";
            }
            return 0;
        }
        std::cout << "[tool] prepare FAILED: "
                  << su::windows::security::logon_secret_error_message(result.error()) << "\n";
        return 1;
    }

    if (command == "stale") {
        auto result = store.mark_stale(sid);
        if (result) {
            std::cout << "[tool] stale: OK\n";
            return 0;
        }
        std::cout << "[tool] stale FAILED: "
                  << su::windows::security::logon_secret_error_message(result.error()) << "\n";
        return 1;
    }

    if (command == "clear") {
        auto result = store.clear(sid);
        if (result) {
            std::cout << "[tool] clear: OK (removed=" << (*result ? "yes" : "no") << ")\n";
            return 0;
        }
        std::cout << "[tool] clear FAILED: "
                  << su::windows::security::logon_secret_error_message(result.error()) << "\n";
        return 1;
    }

    usage();
    return 2;
}
