// HRESULT classification shared by the Media Foundation camera backend and the
// exposure controls (issue #137). Plain header: it includes windows.h, so it
// must never be pulled into a module unit that imports std.
#pragma once

#include <windows.h>
#include <mferror.h>

#include "recognizer/exposure/exposure_control.h"

namespace su::recognizer::detail {

// The camera is held by someone else (or access was refused): worth retrying,
// and for open() it selects the "permission or busy" user message.
[[nodiscard]] constexpr bool is_permission_or_busy(const HRESULT hr) {
    return hr == E_ACCESSDENIED
        || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)
        || hr == HRESULT_FROM_WIN32(ERROR_BUSY)
        || hr == MF_E_VIDEO_DEVICE_LOCKED
        || hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
#ifdef MF_E_VIDEO_RECORDING_DEVICE_LOCKED
        || hr == MF_E_VIDEO_RECORDING_DEVICE_LOCKED
#endif
        ;
}

// Failures that pass on their own: pending work, RPC hiccups and timeouts.
[[nodiscard]] constexpr bool is_transient_hresult(const HRESULT hr) {
    return is_permission_or_busy(hr)
        || hr == E_PENDING
        || hr == RPC_E_CALL_REJECTED
        || hr == RPC_E_SERVERCALL_RETRYLATER
        || hr == RPC_E_TIMEOUT
        || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)
        || hr == HRESULT_FROM_WIN32(ERROR_SEM_TIMEOUT);
}

// DirectShow reports an unsupported property as ERROR_NOT_FOUND
// (E_PROP_ID_UNSUPPORTED), a missing interface as E_NOINTERFACE.
[[nodiscard]] constexpr bool is_unsupported_hresult(const HRESULT hr) {
    return hr == E_NOINTERFACE
        || hr == E_NOTIMPL
        || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
        || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

[[nodiscard]] constexpr ExposureWriteError classify_hresult(const HRESULT hr) {
    if (is_transient_hresult(hr)) {
        return ExposureWriteError::transient;
    }
    return is_unsupported_hresult(hr) ? ExposureWriteError::unsupported
                                      : ExposureWriteError::rejected;
}

}  // namespace su::recognizer::detail
