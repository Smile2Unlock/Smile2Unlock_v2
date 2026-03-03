#pragma once

#include <windows.h>
#include <winerror.h>

namespace CredentialProvider {

/**
 * @brief Credential Provider专用的HRESULT错误码
 * 
 * 使用FACILITY_ITF自定义错误码范围 (0x400-0x4FF)
 */
#define CP_E_BASE 0x400

// 自定义错误码
#define CP_E_MODEL_LOAD_FAILED      MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, CP_E_BASE + 1)
#define CP_E_CAMERA_INIT_FAILED     MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, CP_E_BASE + 2)
#define CP_E_FACE_RECOGNITION_FAILED MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, CP_E_BASE + 3)
#define CP_E_FEATURE_EXTRACTION_FAILED MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, CP_E_BASE + 4)
#define CP_E_CONFIG_LOAD_FAILED     MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, CP_E_BASE + 5)
#define CP_E_NETWORK_COMM_FAILED    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, CP_E_BASE + 6)
#define CP_E_SECURITY_ERROR         MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, CP_E_BASE + 7)
#define CP_E_PROCESS_START_FAILED   MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, CP_E_BASE + 8)

/**
 * @brief 安全的资源释放宏
 */
#define SAFE_RELEASE(p) { if ((p) != nullptr) { (p)->Release(); (p) = nullptr; } }

/**
 * @brief 安全的CoTaskMemFree宏
 */
#define SAFE_COTASK_MEM_FREE(p) { if ((p) != nullptr) { CoTaskMemFree(p); (p) = nullptr; } }

/**
 * @brief 安全的CloseHandle宏
 */
#define SAFE_CLOSE_HANDLE(h) { if ((h) != nullptr && (h) != INVALID_HANDLE_VALUE) { CloseHandle(h); (h) = nullptr; } }

/**
 * @brief 安全的Delete宏
 */
#define SAFE_DELETE(p) { if ((p) != nullptr) { delete (p); (p) = nullptr; } }

/**
 * @brief 安全的Delete[]宏
 */
#define SAFE_DELETE_ARRAY(p) { if ((p) != nullptr) { delete[] (p); (p) = nullptr; } }

/**
 * @brief HRESULT检查宏
 */
#define HR_CHECK(hr) { HRESULT __hr = (hr); if (FAILED(__hr)) { return __hr; } }

/**
 * @brief HRESULT检查并记录日志宏
 */
#define HR_CHECK_LOG(hr, context) { \
    HRESULT __hr = (hr); \
    if (FAILED(__hr)) { \
        LogDebugMessage(L"[ERROR] %s failed: 0x%08X", context, __hr); \
        return __hr; \
    } \
}

} // namespace CredentialProvider