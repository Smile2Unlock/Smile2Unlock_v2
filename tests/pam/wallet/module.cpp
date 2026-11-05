#include <security/pam_modules.h>
#include <cstring>
#include <cstdio>

// Test-only module: seeds a pre-existing token or records what a downstream
// wallet consumes in AUTH. No session entry points are used.
PAM_EXTERN int pam_sm_authenticate(pam_handle_t* handle, int, int argc, const char** argv) {
    const char* output = nullptr;
    for (int index = 0; index < argc; ++index) {
        if (std::strncmp(argv[index], "seed=", 5) == 0)
            return ::pam_set_item(handle, PAM_AUTHTOK, argv[index] + 5);
        if (std::strncmp(argv[index], "output=", 7) == 0)
            output = argv[index] + 7;
    }
    const void* item = nullptr;
    if (!output || ::pam_get_item(handle, PAM_AUTHTOK, &item) != PAM_SUCCESS)
        return PAM_SERVICE_ERR;
    auto* file = std::fopen(output, "w");
    if (!file)
        return PAM_SERVICE_ERR;
    const auto* token = item ? static_cast<const char*>(item) : "";
    const auto length = std::strlen(token);
    const auto written = std::fwrite(token, 1, length, file);
    const auto closed = std::fclose(file);
    return written == length && closed == 0 ? PAM_SUCCESS : PAM_SERVICE_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t*, int, int, const char**) { return PAM_SUCCESS; }
