#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include "rewrite/backend_core.hpp"

extern "C" {

PAM_EXTERN int pam_sm_authenticate(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;

    rewrite::backend::BackendCore backend;
    if (auto init = backend.initialize(); !init) {
        return PAM_AUTHINFO_UNAVAIL;
    }
    return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t* pamh, int flags, int argc, const char** argv) {
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}

} // extern "C"
