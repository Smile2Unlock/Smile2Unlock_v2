//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//

// 必须在其他 Windows 头文件之前包含 Winsock（用于 UDP 通讯）
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#endif
#include <ntstatus.h>
#undef WIN32_NO_STATUS
#include <unknwn.h>
#include "CSampleCredential.h"
#include "guid.h"

// 新增包含
#include "managers/ipc/udp/auth_request_sender.h"
#include "managers/ipc/udp/udp_receiver.h"
#include "models/gui_ipc_protocol.h"
#include "registryhelper.h"
#include "utils/logger.h"
#include "CSampleProvider.h"
#include "hresult_helper.h"
#include "logon_secret_client.h"
#include <chrono>
#include <cwchar>
#include <mutex>
#include <thread>
#include <future>
#include <stdio.h>
#include <stdarg.h>
#include <filesystem>
#include <string>
#include <vector>

#define OUT_DEBUG_TO_FILE 0

namespace {

bool IsAckStatusForRequest(AuthRequestType request_type, RecognitionStatus status) {
  switch (request_type) {
    case AuthRequestType::START_RECOGNITION:
      return status == RecognitionStatus::RECOGNIZING ||
             status == RecognitionStatus::SUCCESS ||
             status == RecognitionStatus::FAILED ||
             status == RecognitionStatus::TIMEOUT ||
             status == RecognitionStatus::RECOGNITION_ERROR ||
             status == RecognitionStatus::PROCESS_ENDED;
    case AuthRequestType::CANCEL_RECOGNITION:
      return status == RecognitionStatus::IDLE ||
             status == RecognitionStatus::PROCESS_ENDED;
    case AuthRequestType::QUERY_STATUS:
      return true;
    default:
      return false;
  }
}

bool IsTerminalFailureStatus(RecognitionStatus status) {
  return status == RecognitionStatus::FAILED ||
         status == RecognitionStatus::TIMEOUT ||
         status == RecognitionStatus::RECOGNITION_ERROR ||
         status == RecognitionStatus::PROCESS_ENDED;
}

void SecureFreePassword(PWSTR& password) {
  if (password == nullptr) {
    return;
  }
  SecureZeroMemory(password, wcslen(password) * sizeof(*password));
  CoTaskMemFree(password);
  password = nullptr;
}

HRESULT ClearPasswordValue(PWSTR& password) {
  PWSTR empty = nullptr;
  const auto copied = SHStrDupW(L"", &empty);
  if (FAILED(copied)) {
    return copied;
  }
  SecureFreePassword(password);
  password = empty;
  return S_OK;
}

bool Utf8ToWideString(const std::string& utf8_text, PWSTR* ppwszText) {
  *ppwszText = nullptr;

  const int bufferSize = MultiByteToWideChar(CP_UTF8, 0, utf8_text.c_str(), -1, nullptr, 0);
  if (bufferSize <= 0) {
    return false;
  }

  PWSTR text = static_cast<PWSTR>(CoTaskMemAlloc(bufferSize * sizeof(wchar_t)));
  if (!text) {
    return false;
  }

  if (!MultiByteToWideChar(CP_UTF8, 0, utf8_text.c_str(), -1, text, bufferSize)) {
    CoTaskMemFree(text);
    return false;
  }

  *ppwszText = text;
  return true;
}

bool IsQualifiedUsername(_In_opt_ PCWSTR username) {
  return username != nullptr &&
         (wcschr(username, L'\\') != nullptr || wcschr(username, L'@') != nullptr);
}

HRESULT BuildQualifiedUsername(_In_ PCWSTR username, _Outptr_result_nullonfailure_ PWSTR* ppwszQualifiedUsername) {
  *ppwszQualifiedUsername = nullptr;

  if (username == nullptr || *username == L'\0') {
    return E_INVALIDARG;
  }

  if (wcschr(username, L'\\') != nullptr) {
    return SHStrDupW(username, ppwszQualifiedUsername);
  }

  if (wcschr(username, L'@') != nullptr) {
    return DomainUsernameStringAlloc(L"MicrosoftAccount", username, ppwszQualifiedUsername);
  }

  WCHAR computer_name[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD cch = ARRAYSIZE(computer_name);
  if (!GetComputerNameW(computer_name, &cch)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }

  return DomainUsernameStringAlloc(computer_name, username, ppwszQualifiedUsername);
}

bool IsSmile2UnlockServiceMutexPresent() {
  HANDLE mutex = OpenMutexA(SYNCHRONIZE, FALSE, smile2unlock::SERVICE_MUTEX_NAME);
  if (mutex != nullptr) {
    CloseHandle(mutex);
    return true;
  }

  const DWORD error = GetLastError();
  return error != ERROR_FILE_NOT_FOUND;
}

}

// 日志辅助函数
inline void LogToEventViewer(const wchar_t* message, WORD wType = EVENTLOG_INFORMATION_TYPE) {
  HANDLE hEventLog = RegisterEventSourceW(nullptr, L"Smile2Unlock_v2");
  if (hEventLog != nullptr) {
    const wchar_t* lpszStrings[1] = { message };
    ReportEventW(hEventLog, wType, 0, 0, nullptr, 1, 0, lpszStrings, nullptr);
    DeregisterEventSource(hEventLog);
  }
}

inline void LogDebugMessage(const wchar_t* format, ...);

inline std::string WideToUtf8LogMessage(const wchar_t* text) {
  if (text == nullptr) {
    return {};
  }

  const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 1) {
    return {};
  }

  std::string utf8(static_cast<size_t>(required - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), required, nullptr, nullptr);
  return utf8;
}

inline std::string ResolveCredentialProviderLogDirectory() {
  const std::string registry_path = RegistryHelper::ReadStringFromRegistry(
      "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\path", "");
  if (!registry_path.empty()) {
    return (std::filesystem::path(registry_path) / "logs").string();
  }

  HMODULE module_handle = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&LogDebugMessage), &module_handle)) {
    return smile2unlock::ResolveLogDirectoryFromModule(module_handle);
  }

  return {};
}

inline void LogDebugMessage(const wchar_t* format, ...) {
  wchar_t buffer[1024];
  va_list args;
  va_start(args, format);
  vswprintf_s(buffer, sizeof(buffer) / sizeof(wchar_t), format, args);
  va_end(args);

  // 输出到调试器
  OutputDebugStringW(buffer);
  OutputDebugStringW(L"\n");

  // 输出到事件日志
  LogToEventViewer(buffer, EVENTLOG_INFORMATION_TYPE);

  static const bool logging_initialized = []() {
    smile2unlock::ConfigureProcessFileLogging("CP", ResolveCredentialProviderLogDirectory());
    return true;
  }();
  (void)logging_initialized;
  smile2unlock::WriteFileLogLine("CP", WideToUtf8LogMessage(buffer));

  // 只在 OUT_DEBUG_TO_FILE 为 1 时输出到文件
#if OUT_DEBUG_TO_FILE
  HANDLE hFile = CreateFileW(L"D:\\Smile2Unlock_v2.log",
                             FILE_APPEND_DATA,
                             FILE_SHARE_READ,
                             nullptr,
                             OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
  if (hFile != INVALID_HANDLE_VALUE) {
    // 添加时间戳
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timeBuffer[256];
    swprintf_s(timeBuffer, sizeof(timeBuffer) / sizeof(wchar_t),
               L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // 转换为UTF-8并写入文件
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, timeBuffer, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Time(utf8Size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, timeBuffer, -1, &utf8Time[0], utf8Size, nullptr, nullptr);

    DWORD bytesWritten;
    WriteFile(hFile, utf8Time.c_str(), (DWORD)utf8Time.size(), &bytesWritten, nullptr);

    utf8Size = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Message(utf8Size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, &utf8Message[0], utf8Size, nullptr, nullptr);

    WriteFile(hFile, utf8Message.c_str(), (DWORD)utf8Message.size(), &bytesWritten, nullptr);
    WriteFile(hFile, "\n", 1, &bytesWritten, nullptr);

    CloseHandle(hFile);
  }
#endif
}

CSampleCredential::CSampleCredential():
    _cRef(1),
    _pCredProvCredentialEvents(nullptr),
    _pszUserSid(nullptr),
    _pszQualifiedUserName(nullptr),
    _fIsLocalUser(false),
    _fChecked(false),
    _fShowControls(false),
    _dwComboIndex(0),
    _hSmile2UnlockProcess(nullptr),
    _dwSmile2UnlockPID(0),
    _serviceGeneration(0),
    _fFaceRecognitionRunning(false),
    _fWarmupModeEnabled(false),
    _fHideCredentialInputFields(false),
    _fFaceCredentialReady(false),
    _fLastSerializationUsedStoredSecret(false),
    _lastSecretRequestId(0),
    _lastSecretSessionId(0),
    _pwzUsername(nullptr),
    _pwzPassword(nullptr),
    _pProvider(nullptr)
{
    DllAddRef();

    ZeroMemory(_rgCredProvFieldDescriptors, sizeof(_rgCredProvFieldDescriptors));
    ZeroMemory(_rgFieldStatePairs, sizeof(_rgFieldStatePairs));
    ZeroMemory(_rgFieldStrings, sizeof(_rgFieldStrings));
}

CSampleCredential::~CSampleCredential()
{
    // 停止人脸识别并清理资源
    StopFaceRecognition();

    // 清理进程句柄
    SAFE_CLOSE_HANDLE(_hSmile2UnlockProcess);

    if (_rgFieldStrings[SFI_PASSWORD])
    {
        size_t lenPassword = wcslen(_rgFieldStrings[SFI_PASSWORD]);
        SecureZeroMemory(_rgFieldStrings[SFI_PASSWORD], lenPassword * sizeof(*_rgFieldStrings[SFI_PASSWORD]));
    }
    for (int i = 0; i < ARRAYSIZE(_rgFieldStrings); i++)
    {
        SAFE_COTASK_MEM_FREE(_rgFieldStrings[i]);
        SAFE_COTASK_MEM_FREE(_rgCredProvFieldDescriptors[i].pszLabel);
    }
    SAFE_COTASK_MEM_FREE(_pszUserSid);
    SAFE_COTASK_MEM_FREE(_pszQualifiedUserName);
    DllRelease();
}


// Initializes one credential with the field information passed in.
// Set the value of the SFI_LARGE_TEXT field to pwzUsername.
HRESULT CSampleCredential::Initialize(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
                                      _In_ CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR const *rgcpfd,
                                      _In_ FIELD_STATE_PAIR const *rgfsp,
                                      _In_opt_ ICredentialProviderUser *pcpUser,
                                      _In_opt_ CSampleProvider *pProvider)
{
    HRESULT hr = S_OK;
    LogDebugMessage(L"[INFO] CSampleCredential::Initialize开始，使用场景: %d", cpus);
    _cpus = cpus;
    _pProvider = pProvider;  // 保存Provider指针

    GUID guidProvider = GUID_NULL;
    if (pcpUser != nullptr)
    {
        pcpUser->GetProviderID(&guidProvider);
        _fIsLocalUser = (guidProvider == Identity_LocalUserProvider);
        LogDebugMessage(L"[INFO] 用户类型: %s", _fIsLocalUser ? L"本地用户" : L"域用户");
    }
    else
    {
        _fIsLocalUser = true;
        LogDebugMessage(L"[INFO] 当前场景未绑定用户对象，按空用户凭证初始化");
    }

    // Copy the field descriptors for each field. This is useful if you want to vary the field
    // descriptors based on what Usage scenario the credential was created for.
    for (DWORD i = 0; i < ARRAYSIZE(_rgCredProvFieldDescriptors); i++)
    {
        _rgFieldStatePairs[i] = rgfsp[i];
        HRESULT hrTemp = FieldDescriptorCopy(rgcpfd[i], &_rgCredProvFieldDescriptors[i]);
        if (FAILED(hrTemp))
        {
            LogDebugMessage(L"[WARNING] FieldDescriptorCopy failed for field %d: 0x%08X", i, hrTemp);
            // 继续处理其他字段，但记录第一个错误
            if (SUCCEEDED(hr)) hr = hrTemp;
        }
    }

    if (_cpus == CPUS_CREDUI)
    {
        _rgFieldStatePairs[SFI_EDIT_TEXT] = { CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_FOCUSED };
        _rgFieldStatePairs[SFI_PASSWORD] = { CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_NONE };
        SHStrDupW(L"用户名", &_rgCredProvFieldDescriptors[SFI_EDIT_TEXT].pszLabel);
        SHStrDupW(L"密码", &_rgCredProvFieldDescriptors[SFI_PASSWORD].pszLabel);
        SHStrDupW(L"提交凭证", &_rgCredProvFieldDescriptors[SFI_SUBMIT_BUTTON].pszLabel);
    }

    // Initialize the String value of all the fields
    // 即使某些操作失败，也继续尝试初始化其他字段，确保Credential Provider处于一致状态
    
    #define INIT_FIELD_STRING(fieldIndex, text, fieldName) \
    do { \
        HRESULT __hr = SHStrDupW(text, &_rgFieldStrings[fieldIndex]); \
        if (FAILED(__hr)) { \
            LogDebugMessage(L"[WARNING] Initialize " fieldName L" failed: 0x%08X", __hr); \
            if (SUCCEEDED(hr)) hr = __hr; \
        } \
    } while(0)
    
    INIT_FIELD_STRING(SFI_LABEL, L"Smile2Unlock", L"SFI_LABEL");
    INIT_FIELD_STRING(SFI_LARGE_TEXT, _cpus == CPUS_CREDUI ? L"Smile2Unlock 凭证提示" : L"Smile2Unlock Credential Provider", L"SFI_LARGE_TEXT");
    INIT_FIELD_STRING(SFI_EDIT_TEXT, L"", L"SFI_EDIT_TEXT");
    INIT_FIELD_STRING(SFI_PASSWORD, L"", L"SFI_PASSWORD");
    INIT_FIELD_STRING(SFI_SUBMIT_BUTTON, L"提交", L"SFI_SUBMIT_BUTTON");
    INIT_FIELD_STRING(SFI_CHECKBOX, L"Checkbox", L"SFI_CHECKBOX");
    INIT_FIELD_STRING(SFI_COMBOBOX, L"Combobox", L"SFI_COMBOBOX");
    INIT_FIELD_STRING(SFI_LAUNCHWINDOW_LINK, L"Launch helper window", L"SFI_LAUNCHWINDOW_LINK");
    INIT_FIELD_STRING(SFI_HIDECONTROLS_LINK, L"Hide additional controls", L"SFI_HIDECONTROLS_LINK");
    
    #undef INIT_FIELD_STRING
    
    HRESULT hrTemp = S_OK;
    if (pcpUser != nullptr)
    {
        hrTemp = pcpUser->GetStringValue(PKEY_Identity_QualifiedUserName, &_pszQualifiedUserName);
        if (FAILED(hrTemp)) {
            LogDebugMessage(L"[WARNING] Get QualifiedUserName failed: 0x%08X", hrTemp);
            if (SUCCEEDED(hr)) hr = hrTemp;
            _pszQualifiedUserName = nullptr;
        }

        PWSTR pszUserName = nullptr;
        hrTemp = pcpUser->GetStringValue(PKEY_Identity_UserName, &pszUserName);
        if (SUCCEEDED(hrTemp) && pszUserName != nullptr)
        {
            wchar_t szString[256];
            StringCchPrintf(szString, ARRAYSIZE(szString), L"User Name: %s", pszUserName);
            hrTemp = SHStrDupW(szString, &_rgFieldStrings[SFI_FULLNAME_TEXT]);
            if (FAILED(hrTemp)) {
                LogDebugMessage(L"[WARNING] Initialize SFI_FULLNAME_TEXT failed: 0x%08X", hrTemp);
                if (SUCCEEDED(hr)) hr = hrTemp;
            }
            SAFE_COTASK_MEM_FREE(pszUserName);
        }
        else
        {
            hrTemp = SHStrDupW(L"User Name is NULL", &_rgFieldStrings[SFI_FULLNAME_TEXT]);
            if (FAILED(hrTemp)) {
                LogDebugMessage(L"[WARNING] Initialize SFI_FULLNAME_TEXT (NULL) failed: 0x%08X", hrTemp);
                if (SUCCEEDED(hr)) hr = hrTemp;
            }
            SAFE_COTASK_MEM_FREE(pszUserName);
        }

        PWSTR pszDisplayName = nullptr;
        hrTemp = pcpUser->GetStringValue(PKEY_Identity_DisplayName, &pszDisplayName);
        if (SUCCEEDED(hrTemp) && pszDisplayName != nullptr)
        {
            wchar_t szString[256];
            StringCchPrintf(szString, ARRAYSIZE(szString), L"Display Name: %s", pszDisplayName);
            hrTemp = SHStrDupW(szString, &_rgFieldStrings[SFI_DISPLAYNAME_TEXT]);
            if (FAILED(hrTemp)) {
                LogDebugMessage(L"[WARNING] Initialize SFI_DISPLAYNAME_TEXT failed: 0x%08X", hrTemp);
                if (SUCCEEDED(hr)) hr = hrTemp;
            }
            SAFE_COTASK_MEM_FREE(pszDisplayName);
        }
        else
        {
            hrTemp = SHStrDupW(L"Display Name is NULL", &_rgFieldStrings[SFI_DISPLAYNAME_TEXT]);
            if (FAILED(hrTemp)) {
                LogDebugMessage(L"[WARNING] Initialize SFI_DISPLAYNAME_TEXT (NULL) failed: 0x%08X", hrTemp);
                if (SUCCEEDED(hr)) hr = hrTemp;
            }
            SAFE_COTASK_MEM_FREE(pszDisplayName);
        }

        PWSTR pszLogonStatus = nullptr;
        hrTemp = pcpUser->GetStringValue(PKEY_Identity_LogonStatusString, &pszLogonStatus);
        if (SUCCEEDED(hrTemp) && pszLogonStatus != nullptr)
        {
            wchar_t szString[256];
            StringCchPrintf(szString, ARRAYSIZE(szString), L"Logon Status: %s", pszLogonStatus);
            hrTemp = SHStrDupW(szString, &_rgFieldStrings[SFI_LOGONSTATUS_TEXT]);
            if (FAILED(hrTemp)) {
                LogDebugMessage(L"[WARNING] Initialize SFI_LOGONSTATUS_TEXT failed: 0x%08X", hrTemp);
                if (SUCCEEDED(hr)) hr = hrTemp;
            }
            SAFE_COTASK_MEM_FREE(pszLogonStatus);
        }
        else
        {
            hrTemp = SHStrDupW(L"Logon Status is NULL", &_rgFieldStrings[SFI_LOGONSTATUS_TEXT]);
            if (FAILED(hrTemp)) {
                LogDebugMessage(L"[WARNING] Initialize SFI_LOGONSTATUS_TEXT (NULL) failed: 0x%08X", hrTemp);
                if (SUCCEEDED(hr)) hr = hrTemp;
            }
            SAFE_COTASK_MEM_FREE(pszLogonStatus);
        }

        hrTemp = pcpUser->GetSid(&_pszUserSid);
        if (FAILED(hrTemp)) {
            LogDebugMessage(L"[WARNING] Get User SID failed: 0x%08X", hrTemp);
            if (SUCCEEDED(hr)) hr = hrTemp;
            _pszUserSid = nullptr;
        }
    }
    else
    {
        SHStrDupW(L"", &_rgFieldStrings[SFI_FULLNAME_TEXT]);
        SHStrDupW(L"", &_rgFieldStrings[SFI_DISPLAYNAME_TEXT]);
        SHStrDupW(L"", &_rgFieldStrings[SFI_LOGONSTATUS_TEXT]);
    }

    // 初始化 _pwzUsername 和 _pwzPassword（来自 Sparkin 实现）
    if (_pszQualifiedUserName)
    {
        // 从 QualifiedUserName 中提取用户名部分（可能是 domain\username 或 MicrosoftAccount\email）
        PWSTR pwzBackslash = wcschr(_pszQualifiedUserName, L'\\');
        if (pwzBackslash)
        {
            // 使用反斜杠后的部分作为用户名
            _pwzUsername = pwzBackslash + 1;
            LogDebugMessage(L"[INFO] 从QualifiedUserName提取用户名: %s", _pwzUsername);
        }
        else
        {
            // 如果没有反斜杠，直接使用整个字符串
            _pwzUsername = _pszQualifiedUserName;
            LogDebugMessage(L"[INFO] 使用整个QualifiedUserName作为用户名: %s", _pwzUsername);
        }
    }
    else if (_cpus == CPUS_CREDUI)
    {
        _pwzUsername = _rgFieldStrings[SFI_EDIT_TEXT];
        LogDebugMessage(L"[INFO] CredUI 场景使用编辑框输入用户名");
    }
    
    // _pwzPassword 在 GetSerialization 时从密码字段获取
    _pwzPassword = nullptr;

    // 初始化面部识别链接字段
    HRESULT hrTemp2 = SHStrDupW(L"使用面部识别登录", &_rgFieldStrings[SFI_FACE_RECOGNITION_LINK]);
    if (FAILED(hrTemp2)) {
        LogDebugMessage(L"[WARNING] Initialize SFI_FACE_RECOGNITION_LINK failed: 0x%08X", hrTemp2);
        if (SUCCEEDED(hr)) hr = hrTemp2;
    }

    // 从注册表读取预热模式配置（自动启动已移除，始终自动启动）
    _fWarmupModeEnabled = RegistryHelper::ReadDwordFromRegistry(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\warmup_mode", 0) != 0;
    LogDebugMessage(L"[INFO] 从注册表读取配置: warmup_mode=%d (自动启动已启用)",
            _fWarmupModeEnabled);

    // 记录初始化结果
    if (SUCCEEDED(hr)) {
        LogDebugMessage(L"[INFO] CSampleCredential::Initialize成功完成");
    } else {
        LogDebugMessage(L"[WARNING] CSampleCredential::Initialize完成但有错误: 0x%08X", hr);
    }
    
    // 始终返回S_OK以确保Credential Provider可用，即使有非致命错误
    return S_OK;
}

// LogonUI calls this in order to give us a callback in case we need to notify it of anything.
HRESULT CSampleCredential::Advise(_In_ ICredentialProviderCredentialEvents *pcpce)
{
    LogDebugMessage(L"[INFO] CSampleCredential::Advise - 初始化UDP接收器");

    if (_pCredProvCredentialEvents != nullptr)
    {
        _pCredProvCredentialEvents->Release();
    }

    // 初始化UDP接收器
    if (!_pUdpReceiver)
    {
        LogDebugMessage(L"[INFO] 正在创建UDP接收器...");
        _pUdpReceiver = std::make_unique<UdpReceiver>();
        LogDebugMessage(L"[INFO] UDP接收器创建成功，接收线程应已启动");
        // 给接收器线程一点时间来连接
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        LogDebugMessage(L"[INFO] Advise已完成");
    }
    else
    {
        LogDebugMessage(L"[INFO] UDP接收器已存在，重用现有实例");
    }

    return pcpce->QueryInterface(IID_PPV_ARGS(&_pCredProvCredentialEvents));
}

// LogonUI calls this to tell us to release the callback.
HRESULT CSampleCredential::UnAdvise()
{
    LogDebugMessage(L"[INFO] CSampleCredential::UnAdvise - 释放事件回调与UDP接收器");

    StopFaceRecognition();

    if (_pCredProvCredentialEvents)
    {
        _pCredProvCredentialEvents->Release();
    }
    _pCredProvCredentialEvents = nullptr;

    // UDP接收器会在 unique_ptr 析构时自动清理
    _pUdpReceiver.reset();

    return S_OK;
}

// LogonUI calls this function when our tile is selected (zoomed)
// If you simply want fields to show/hide based on the selected state,
// there's no need to do anything here - you can set that up in the
// field definitions. But if you want to do something
// more complicated, like change the contents of a field when the tile is
// selected, you would do it here.
HRESULT CSampleCredential::SetSelected(_Out_ BOOL *pbAutoLogon)
{
    *pbAutoLogon = FALSE;
    _fHideCredentialInputFields = false;

    if (_cpus == CPUS_CREDUI)
    {
        _rgFieldStatePairs[SFI_EDIT_TEXT] = { CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_FOCUSED };
        _rgFieldStatePairs[SFI_PASSWORD] = { CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_NONE };
        _rgFieldStatePairs[SFI_SUBMIT_BUTTON] = { CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_NONE };
        return S_OK;
    }

    extern std::atomic<RecognitionStatus> face_recognition_status;
    extern std::atomic<ULONGLONG> face_recognition_status_tick;

    LogDebugMessage(L"[INFO] CSampleCredential::SetSelected - status=%d tick=%llu thread_joinable=%d process_alive=%d",
                    static_cast<int>(face_recognition_status.load()),
                    face_recognition_status_tick.load(std::memory_order_relaxed),
                    _faceRecognitionThread.joinable() ? 1 : 0,
                    (_hSmile2UnlockProcess != nullptr && IsProcessStillRunning()) ? 1 : 0);

    if (_pProvider) {
        _pProvider->ResetCredentialReady();
    }

    // 始终启动人脸识别（已移除 _fAutoStartEnabled 检查）
    // 检查是否已在运行
    bool alreadyRunning = false;
    {
        std::lock_guard<std::mutex> lock(_faceMutex);
        alreadyRunning = _fFaceRecognitionRunning;
    }

    if (!alreadyRunning) {
        LogDebugMessage(L"[INFO] 开始自动人脸识别");
        StartFaceRecognitionAsync();
    } else {
        LogDebugMessage(L"[INFO] 人脸识别已在运行");
    }

    return S_OK;
}

// Similarly to SetSelected, LogonUI calls this when your tile was selected
// and now no longer is. The most common thing to do here (which we do below)
// is to clear out the password field.
HRESULT CSampleCredential::SetDeselected()
{
    LogDebugMessage(L"[INFO] CSampleCredential::SetDeselected - 开始清理当前识别会话");
    {
        std::lock_guard<std::mutex> lock(_faceMutex);
        _fHideCredentialInputFields = false;
        _fFaceCredentialReady = false;
    }
    _fLastSerializationUsedStoredSecret = false;
    _lastSecretRequestId = 0;
    _lastSecretSessionId = 0;
    if (_pProvider) {
        _pProvider->ResetCredentialReady();
    }

    // 停止人脸识别
    StopFaceRecognition();

    HRESULT hr = S_OK;
    if (_rgFieldStrings[SFI_PASSWORD])
    {
        size_t lenPassword = wcslen(_rgFieldStrings[SFI_PASSWORD]);
        SecureZeroMemory(_rgFieldStrings[SFI_PASSWORD], lenPassword * sizeof(*_rgFieldStrings[SFI_PASSWORD]));

        CoTaskMemFree(_rgFieldStrings[SFI_PASSWORD]);
        _rgFieldStrings[SFI_PASSWORD] = nullptr;
        
        hr = SHStrDupW(L"", &_rgFieldStrings[SFI_PASSWORD]);

        if (SUCCEEDED(hr) && _pCredProvCredentialEvents)
        {
            _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, _rgFieldStrings[SFI_PASSWORD]);
        }
    }

    return hr;
}

// Get info for a particular field of a tile. Called by logonUI to get information
// to display the tile.
HRESULT CSampleCredential::GetFieldState(DWORD dwFieldID,
                                         _Out_ CREDENTIAL_PROVIDER_FIELD_STATE *pcpfs,
                                         _Out_ CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE *pcpfis)
{
    HRESULT hr;

    // Validate our parameters.
    if ((dwFieldID < ARRAYSIZE(_rgFieldStatePairs)))
    {
        *pcpfs = _rgFieldStatePairs[dwFieldID].cpfs;
        *pcpfis = _rgFieldStatePairs[dwFieldID].cpfis;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }
    return hr;
}

// Sets ppwsz to the string value of the field at the index dwFieldID
HRESULT CSampleCredential::GetStringValue(DWORD dwFieldID, _Outptr_result_nullonfailure_ PWSTR *ppwsz)
{
    HRESULT hr;
    *ppwsz = nullptr;

    // Check to make sure dwFieldID is a legitimate index
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors))
    {
        // Make a copy of the string and return that. The caller
        // is responsible for freeing it.
        hr = SHStrDupW(_rgFieldStrings[dwFieldID], ppwsz);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Get the image to show in the user tile
HRESULT CSampleCredential::GetBitmapValue(DWORD dwFieldID, _Outptr_result_nullonfailure_ HBITMAP *phbmp)
{
    HRESULT hr;
    *phbmp = nullptr;

    if ((SFI_TILEIMAGE == dwFieldID))
    {
        HBITMAP hbmp = LoadBitmap(HINST_THISDLL, MAKEINTRESOURCE(IDB_TILE_IMAGE));
        if (hbmp != nullptr)
        {
            hr = S_OK;
            *phbmp = hbmp;
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Sets pdwAdjacentTo to the index of the field the submit button should be
// adjacent to. We recommend that the submit button is placed next to the last
// field which the user is required to enter information in. Optional fields
// should be below the submit button.
HRESULT CSampleCredential::GetSubmitButtonValue(DWORD dwFieldID, _Out_ DWORD *pdwAdjacentTo)
{
    HRESULT hr;

    if (SFI_SUBMIT_BUTTON == dwFieldID)
    {
        // pdwAdjacentTo is a pointer to the fieldID you want the submit button to
        // appear next to.
        *pdwAdjacentTo = SFI_PASSWORD;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }
    return hr;
}

// Sets the value of a field which can accept a string as a value.
// This is called on each keystroke when a user types into an edit field
HRESULT CSampleCredential::SetStringValue(DWORD dwFieldID, _In_ PCWSTR pwz)
{
    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_EDIT_TEXT == _rgCredProvFieldDescriptors[dwFieldID].cpft ||
        CPFT_PASSWORD_TEXT == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        PWSTR *ppwszStored = &_rgFieldStrings[dwFieldID];
        SAFE_COTASK_MEM_FREE(*ppwszStored);
        HRESULT hr = SHStrDupW(pwz, ppwszStored);
        if (FAILED(hr)) {
            LogDebugMessage(L"[ERROR] SetStringValue failed: 0x%08X", hr);
            return hr;
        }

        if (dwFieldID == SFI_EDIT_TEXT)
        {
            _pwzUsername = _rgFieldStrings[SFI_EDIT_TEXT];
        }
        else if (dwFieldID == SFI_PASSWORD)
        {
            _pwzPassword = _rgFieldStrings[SFI_PASSWORD];
        }

        return S_OK;
    }
    
    return E_INVALIDARG;
}

// Returns whether a checkbox is checked or not as well as its label.
HRESULT CSampleCredential::GetCheckboxValue(DWORD dwFieldID, _Out_ BOOL *pbChecked, _Outptr_result_nullonfailure_ PWSTR *ppwszLabel)
{
    HRESULT hr;
    *ppwszLabel = nullptr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_CHECKBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        *pbChecked = _fChecked;
        hr = SHStrDupW(_rgFieldStrings[SFI_CHECKBOX], ppwszLabel);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Sets whether the specified checkbox is checked or not.
HRESULT CSampleCredential::SetCheckboxValue(DWORD dwFieldID, BOOL bChecked)
{
    HRESULT hr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_CHECKBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        _fChecked = bChecked;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Returns the number of items to be included in the combobox (pcItems), as well as the
// currently selected item (pdwSelectedItem).
HRESULT CSampleCredential::GetComboBoxValueCount(DWORD dwFieldID, _Out_ DWORD *pcItems, _Deref_out_range_(<, *pcItems) _Out_ DWORD *pdwSelectedItem)
{
    HRESULT hr;
    *pcItems = 0;
    *pdwSelectedItem = 0;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        *pcItems = ARRAYSIZE(s_rgComboBoxStrings);
        *pdwSelectedItem = 0;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Called iteratively to fill the combobox with the string (ppwszItem) at index dwItem.
HRESULT CSampleCredential::GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, _Outptr_result_nullonfailure_ PWSTR *ppwszItem)
{
    HRESULT hr;
    *ppwszItem = nullptr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        hr = SHStrDupW(s_rgComboBoxStrings[dwItem], ppwszItem);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Called when the user changes the selected item in the combobox.
HRESULT CSampleCredential::SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem)
{
    HRESULT hr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMBOBOX == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        _dwComboIndex = dwSelectedItem;
        hr = S_OK;
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// ============ 人脸识别辅助函数实现 ============

HRESULT CSampleCredential::LaunchSmile2UnlockService() {
  std::unique_lock<std::mutex> service_lock(_serviceMutex);

  // 1. 检查现有进程是否仍在运行
  if (_hSmile2UnlockProcess != nullptr) {
    DWORD dwExitCode;
    if (GetExitCodeProcess(_hSmile2UnlockProcess, &dwExitCode)) {
      if (dwExitCode == STILL_ACTIVE) {
        LogDebugMessage(L"[INFO] Smile2Unlock服务进程仍在运行 (PID: %u)，复用现有进程", _dwSmile2UnlockPID);
        return S_OK; // 复用现有进程
      }
    }
    // 进程已结束，清理旧句柄
    LogDebugMessage(L"[INFO] 检测到旧进程已结束，清理句柄");
    CloseHandle(_hSmile2UnlockProcess);
    _hSmile2UnlockProcess = nullptr;
    _dwSmile2UnlockPID = 0;
  }

  if (IsSmile2UnlockServiceMutexPresent()) {
    LogDebugMessage(L"[INFO] 检测到已存在的 Smile2Unlock 服务实例，复用外部服务");
    return S_OK;
  }

  // 2. 获取 Smile2Unlock.exe 的路径
  wchar_t szModuleDir[MAX_PATH] = {0};
  wchar_t szExePath[MAX_PATH] = {0};

  // 从注册表读取路径
  std::string registryPath = RegistryHelper::ReadStringFromRegistry(
    "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\path", "");

  bool useRegistryPath = false;
  if (!registryPath.empty()) {
    int len = MultiByteToWideChar(CP_UTF8, 0, registryPath.c_str(), -1, szModuleDir, MAX_PATH);
    if (len > 0) {
      size_t pathLen = wcslen(szModuleDir);
      if (pathLen > 0 && szModuleDir[pathLen - 1] != L'\\') {
        wcscat_s(szModuleDir, MAX_PATH, L"\\");
      }
      useRegistryPath = true;
      LogDebugMessage(L"[INFO] 从注册表读取 Smile2Unlock 安装路径: %s", szModuleDir);
    }
  }

  if (!useRegistryPath) {
    LogDebugMessage(L"[WARNING] 未能从注册表读取路径，使用DLL所在目录");
    wchar_t szSmile2UnlockPath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, szSmile2UnlockPath, MAX_PATH)) {
      LogDebugMessage(L"[ERROR] 获取模块路径失败");
      return HRESULT_FROM_WIN32(GetLastError());
    }
    wcscpy_s(szModuleDir, MAX_PATH, szSmile2UnlockPath);
    wchar_t *pLastSlash = wcsrchr(szModuleDir, L'\\');
    if (pLastSlash) {
      *(pLastSlash + 1) = L'\0';
    }
  }

  // 3. 拼接exe路径并检查
  wcscpy_s(szExePath, MAX_PATH, szModuleDir);
  wcscat_s(szExePath, MAX_PATH, L"Smile2Unlock.exe");

  DWORD attrib = GetFileAttributesW(szExePath);
  if (attrib == INVALID_FILE_ATTRIBUTES) {
    LogDebugMessage(L"[ERROR] Smile2Unlock.exe不存在: %s", szExePath);
    return E_FAIL;
  }

  // 4. 准备启动参数
  wchar_t szCmdLine[MAX_PATH * 2];
  swprintf_s(szCmdLine, MAX_PATH * 2, L"\"%s\" --service", szExePath);
  LogDebugMessage(L"[INFO] 启动命令行: %s", szCmdLine);

  // 5. 创建进程
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  if (!CreateProcessW(nullptr, szCmdLine, nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, szModuleDir, &si, &pi)) {
    DWORD dwError = GetLastError();
    LogDebugMessage(L"[ERROR] CreateProcessW失败，错误代码: 0x%08X", dwError);
    return HRESULT_FROM_WIN32(dwError);
  }

  // 6. 保存进程信息
  _hSmile2UnlockProcess = pi.hProcess;
  _dwSmile2UnlockPID = pi.dwProcessId;
  LogDebugMessage(L"[INFO] Smile2Unlock服务进程已启动，PID: %u", pi.dwProcessId);

  CloseHandle(pi.hThread);
  HANDLE process_handle = _hSmile2UnlockProcess;
  service_lock.unlock();

  // 刚开机时 SU 冷启动较慢，给它一个短暂的自检/绑定窗口，避免过早发送首个 START 请求。
  constexpr DWORD kStartupGraceMs = 800;
  constexpr DWORD kStartupPollMs = 50;
  for (DWORD waited = 0; waited < kStartupGraceMs; waited += kStartupPollMs) {
    DWORD dwExitCode = STILL_ACTIVE;
    if (!GetExitCodeProcess(process_handle, &dwExitCode)) {
      break;
    }

    if (dwExitCode != STILL_ACTIVE) {
      LogDebugMessage(L"[ERROR] Smile2Unlock 在冷启动等待阶段提前退出，退出代码: %d", dwExitCode);

      std::lock_guard<std::mutex> relock(_serviceMutex);
      if (_hSmile2UnlockProcess == process_handle) {
        CloseHandle(_hSmile2UnlockProcess);
        _hSmile2UnlockProcess = nullptr;
        _dwSmile2UnlockPID = 0;
      }
      return E_FAIL;
    }

    Sleep(kStartupPollMs);
  }

  LogDebugMessage(L"[INFO] Smile2Unlock 冷启动等待完成，准备发送识别请求");
  return S_OK;
}

HRESULT CSampleCredential::SendAuthRequestToSmile2Unlock(AuthRequestType request_type) {
  AuthRequestSender sender("127.0.0.1", 51236);
  extern std::atomic<RecognitionStatus> face_recognition_status;
  extern std::atomic<ULONGLONG> face_recognition_status_tick;

  std::string username_hint;
  if (_pwzUsername != nullptr) {
    int required = WideCharToMultiByte(CP_UTF8, 0, _pwzUsername, -1, nullptr, 0, nullptr, nullptr);
    if (required > 1) {
      username_hint.resize(static_cast<size_t>(required));
      WideCharToMultiByte(CP_UTF8, 0, _pwzUsername, -1, username_hint.data(), required, nullptr, nullptr);
      username_hint.resize(static_cast<size_t>(required - 1));
    }
  }

  constexpr int kMaxAttempts = 10;
  constexpr ULONGLONG kAckWaitMs = 500;
  LogDebugMessage(L"[INFO] SendAuthRequestToSmile2Unlock - type=%d username_hint=%hs status_before=%d tick_before=%llu",
                  static_cast<int>(request_type),
                  username_hint.empty() ? "" : username_hint.c_str(),
                  static_cast<int>(face_recognition_status.load()),
                  face_recognition_status_tick.load(std::memory_order_relaxed));
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const ULONGLONG tick_before_send = face_recognition_status_tick.load(std::memory_order_relaxed);

    if (sender.send_request(request_type, username_hint, GetTickCount())) {
      LogDebugMessage(L"[INFO] 已向 Smile2Unlock 发送认证请求，type=%d, attempt=%d",
                      static_cast<int>(request_type), attempt + 1);

      if (request_type == AuthRequestType::QUERY_STATUS) {
        return S_OK;
      }

      const ULONGLONG wait_start = GetTickCount64();
      while (GetTickCount64() - wait_start < kAckWaitMs) {
        const ULONGLONG current_tick = face_recognition_status_tick.load(std::memory_order_relaxed);
        const RecognitionStatus current_status = face_recognition_status.load();

        if (current_tick > tick_before_send && IsAckStatusForRequest(request_type, current_status)) {
          LogDebugMessage(L"[INFO] Smile2Unlock 已确认请求，type=%d, status=%d, attempt=%d",
                          static_cast<int>(request_type), static_cast<int>(current_status), attempt + 1);
          return S_OK;
        }

        if (_hSmile2UnlockProcess != nullptr && !IsProcessStillRunning()) {
          LogDebugMessage(L"[ERROR] 等待请求确认时检测到 Smile2Unlock 已退出，type=%d",
                          static_cast<int>(request_type));
          return E_FAIL;
        }

        Sleep(20);
      }

      LogDebugMessage(L"[WARNING] 请求已发送但未收到确认状态，准备重试，type=%d, attempt=%d",
                      static_cast<int>(request_type), attempt + 1);
    }

    Sleep(200);
  }

  LogDebugMessage(L"[ERROR] 向 Smile2Unlock 发送认证请求失败，type=%d", static_cast<int>(request_type));
  return E_FAIL;
}

HRESULT CSampleCredential::WaitForFaceRecognitionResult() {
  const int CHECK_INTERVAL_MS = 100;        // 每 100ms 检查一次
  int elapsed = 0;

  {
    std::lock_guard<std::mutex> lock(_faceMutex);
    _fFaceRecognitionRunning = true;
  }

  // 外部全局变量,由UDP接收器更新
  extern std::atomic<RecognitionStatus> face_recognition_status;

  LogDebugMessage(L"[INFO] 开始等待人脸识别结果，无超时限制");
  LogDebugMessage(L"[DEBUG] 初始状态码：%d", static_cast<int>(face_recognition_status.load()));
  
  while (true) {
    {
      std::lock_guard<std::mutex> lock(_faceMutex);
      if (!_fFaceRecognitionRunning) {
        LogDebugMessage(L"[INFO] 识别被取消");
        break;
      }
    }

    // 检查识别结果（RecognitionStatus::SUCCESS = 2）
    RecognitionStatus currentStatus = face_recognition_status.load();
    if (currentStatus == RecognitionStatus::SUCCESS) {
      LogDebugMessage(L"[INFO] 识别成功！耗时: %dms，最终状态码: %d", elapsed, static_cast<int>(currentStatus));
      return S_OK; // 识别成功
    }
    if (IsTerminalFailureStatus(currentStatus)) {
      LogDebugMessage(L"[INFO] 识别已被SU拒绝或结束，耗时: %dms，最终状态码: %d", elapsed, static_cast<int>(currentStatus));
      return E_FAIL;
    }

    // 每秒输出一次状态日志
    if (elapsed % 1000 == 0 && elapsed > 0) {
      LogDebugMessage(L"[DEBUG] 等待中... 已耗时：%dms，当前状态码：%d",
                      elapsed, static_cast<int>(currentStatus));
    }

    Sleep(CHECK_INTERVAL_MS);
    elapsed += CHECK_INTERVAL_MS;
  }

  LogDebugMessage(L"[ERROR] 人脸识别被取消或进程异常退出，最后状态码：%d", 
                  static_cast<int>(face_recognition_status.load()));
  return E_FAIL;
}

HRESULT CSampleCredential::RequestOneTimeLogonSecret(PWSTR *ppwszPassword) {
  *ppwszPassword = nullptr;
  if (_pszUserSid == nullptr || _pszUserSid[0] == L'\0') {
    return E_INVALIDARG;
  }
  auto session_id = DWORD{0};
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  const auto request_id =
      (_serviceGeneration.load(std::memory_order_acquire) << 32) ^ GetTickCount64();
  auto prepared = LogonSecretClient{}.prepare(_pszUserSid, request_id, session_id);
  if (!prepared) {
    return prepared.error();
  }
  const auto copied = SHStrDupW(prepared->c_str(), ppwszPassword);
  if (SUCCEEDED(copied)) {
    _lastSecretRequestId = request_id;
    _lastSecretSessionId = session_id;
  }
  return copied;
}

HRESULT CSampleCredential::CommandLinkClicked(DWORD dwFieldID)
{
    HRESULT hr = S_OK;
    CREDENTIAL_PROVIDER_FIELD_STATE cpfsShow = CPFS_HIDDEN;

    // Validate parameter.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMMAND_LINK == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        HWND hwndOwner = nullptr;

        switch (dwFieldID)
        {
        case SFI_LAUNCHWINDOW_LINK:
            if (_pCredProvCredentialEvents)
            {
                _pCredProvCredentialEvents->OnCreatingWindow(&hwndOwner);
            }

            // Pop a messagebox indicating the click.
            ::MessageBox(hwndOwner, L"Command link clicked", L"Click!", 0);
            break;
        case SFI_HIDECONTROLS_LINK:
            _pCredProvCredentialEvents->BeginFieldUpdates();
            cpfsShow = _fShowControls ? CPFS_DISPLAY_IN_SELECTED_TILE : CPFS_HIDDEN;
            _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_FULLNAME_TEXT, cpfsShow);
            _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_DISPLAYNAME_TEXT, cpfsShow);
            _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_LOGONSTATUS_TEXT, cpfsShow);
            _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_CHECKBOX, cpfsShow);
            _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_EDIT_TEXT, cpfsShow);
            _pCredProvCredentialEvents->SetFieldState(nullptr, SFI_COMBOBOX, cpfsShow);
            _pCredProvCredentialEvents->SetFieldString(nullptr, SFI_HIDECONTROLS_LINK, _fShowControls? L"Hide additional controls" : L"Show additional controls");
            _pCredProvCredentialEvents->EndFieldUpdates();
            _fShowControls = !_fShowControls;
            break;
        case SFI_FACE_RECOGNITION_LINK:
            if (_pCredProvCredentialEvents) {
                _pCredProvCredentialEvents->OnCreatingWindow(&hwndOwner);
            }

            LogDebugMessage(L"[INFO] 用户点击人脸识别按钮");

            // 检查是否已在运行
            {
                std::lock_guard<std::mutex> lock(_faceMutex);
                if (_fFaceRecognitionRunning) {
                    LogDebugMessage(L"[INFO] 人脸识别已在运行，忽略点击");
                    ::MessageBox(hwndOwner, L"人脸识别正在进行中，请稍候...", L"提示", MB_ICONINFORMATION);
                    break;
                }
            }

            // 重置识别状态
            extern std::atomic<RecognitionStatus> face_recognition_status;
            face_recognition_status = RecognitionStatus::IDLE;

            // 显示等待提示
            if (_pCredProvCredentialEvents) {
                _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT,
                                                           L"正在进行人脸识别...");
            }

            // 启动异步识别
            hr = StartFaceRecognitionAsync();
            if (FAILED(hr)) {
                LogDebugMessage(L"[ERROR] 启动人脸识别失败: 0x%08X", hr);
                ::MessageBox(hwndOwner, L"启动人脸识别失败", L"错误", MB_ICONERROR);
            }

            break;
        default:
            hr = E_INVALIDARG;
        }

    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Collect the username and password into a serialized credential for the correct usage scenario
// (logon/unlock is what's demonstrated in this sample).  LogonUI then passes these credentials
// back to the system to log on.
HRESULT CSampleCredential::GetSerialization(_Out_ CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE *pcpgsr,
                                            _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs,
                                            _Outptr_result_maybenull_ PWSTR *ppwszOptionalStatusText,
                                            _Out_ CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon)
{
    HRESULT hr = E_UNEXPECTED;
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    ZeroMemory(pcpcs, sizeof(*pcpcs));
    _fLastSerializationUsedStoredSecret = false;

    LogDebugMessage(L"[INFO] GetSerialization被调用");
    LogDebugMessage(L"[DEBUG] _pwzUsername: %s", _pwzUsername ? _pwzUsername : L"NULL");
    LogDebugMessage(L"[DEBUG] 密码字段内容: %s", _rgFieldStrings[SFI_PASSWORD] ? L"已填写" : L"为空");
    LogDebugMessage(L"[DEBUG] 密码字段长度: %u", _rgFieldStrings[SFI_PASSWORD] ? static_cast<unsigned>(wcslen(_rgFieldStrings[SFI_PASSWORD])) : 0);
    LogDebugMessage(L"[DEBUG] QualifiedUserName: %s", _pszQualifiedUserName ? _pszQualifiedUserName : L"NULL");

    // CredUI only supports the password entered into the visible system field.
    _pwzPassword = _rgFieldStrings[SFI_PASSWORD];

    if (_cpus == CPUS_CREDUI)
    {
        const PWSTR pwzCredUiUsername = (_pwzUsername != nullptr) ? _pwzUsername : _rgFieldStrings[SFI_EDIT_TEXT];
        if (pwzCredUiUsername == nullptr || *pwzCredUiUsername == L'\0')
        {
            LogDebugMessage(L"[ERROR] CredUI 场景缺少用户名");
            *pcpsiOptionalStatusIcon = CPSI_ERROR;
            SHStrDupW(L"请输入用户名。", ppwszOptionalStatusText);
            return E_INVALIDARG;
        }

        PWSTR pwzPackedUsername = nullptr;
        if (IsQualifiedUsername(pwzCredUiUsername))
        {
            hr = SHStrDupW(pwzCredUiUsername, &pwzPackedUsername);
        }
        else
        {
            hr = BuildQualifiedUsername(pwzCredUiUsername, &pwzPackedUsername);
        }
        if (FAILED(hr) || pwzPackedUsername == nullptr)
        {
            LogDebugMessage(L"[ERROR] CredUI 用户名规范化失败: 0x%08X", hr);
            return FAILED(hr) ? hr : E_FAIL;
        }

        DWORD cbSerialization = 0;
        if (!CredPackAuthenticationBufferW(0, pwzPackedUsername, _pwzPassword, nullptr, &cbSerialization))
        {
            const DWORD dwErr = GetLastError();
            if (dwErr != ERROR_INSUFFICIENT_BUFFER)
            {
                hr = HRESULT_FROM_WIN32(dwErr);
                LogDebugMessage(L"[ERROR] CredPackAuthenticationBufferW 预计算失败: 0x%08X", hr);
                CoTaskMemFree(pwzPackedUsername);
                return hr;
            }
        }

        pcpcs->rgbSerialization = static_cast<BYTE*>(CoTaskMemAlloc(cbSerialization));
        if (pcpcs->rgbSerialization == nullptr)
        {
            LogDebugMessage(L"[ERROR] CredUI 序列化缓冲区分配失败");
            CoTaskMemFree(pwzPackedUsername);
            return E_OUTOFMEMORY;
        }

        if (!CredPackAuthenticationBufferW(0, pwzPackedUsername, _pwzPassword, pcpcs->rgbSerialization, &cbSerialization))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            CoTaskMemFree(pcpcs->rgbSerialization);
            pcpcs->rgbSerialization = nullptr;
            LogDebugMessage(L"[ERROR] CredPackAuthenticationBufferW 失败: 0x%08X", hr);
            CoTaskMemFree(pwzPackedUsername);
            return hr;
        }

        pcpcs->cbSerialization = cbSerialization;
        hr = RetrieveNegotiateAuthPackage(&pcpcs->ulAuthenticationPackage);
        if (SUCCEEDED(hr))
        {
            pcpcs->clsidCredentialProvider = CLSID_CSample;
            *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
            LogDebugMessage(L"[INFO] CredUI 序列化成功，原始用户名: %s, 打包用户名: %s", pwzCredUiUsername, pwzPackedUsername);
        }
        else
        {
            CoTaskMemFree(pcpcs->rgbSerialization);
            pcpcs->rgbSerialization = nullptr;
            pcpcs->cbSerialization = 0;
            LogDebugMessage(L"[ERROR] RetrieveNegotiateAuthPackage失败");
        }
        CoTaskMemFree(pwzPackedUsername);
    }
    else
    {
        PWSTR pwzDomain = nullptr;
        PWSTR pwzUsername = nullptr;
        PWSTR pwzOneTimePassword = nullptr;
        PWSTR pwzProtectedPassword = nullptr;
        bool useStoredSecret = false;

        {
            std::lock_guard<std::mutex> lock(_faceMutex);
            useStoredSecret = _fFaceCredentialReady
                && (_cpus == CPUS_LOGON || _cpus == CPUS_UNLOCK_WORKSTATION);
        }

        if (_pszQualifiedUserName != nullptr && wcschr(_pszQualifiedUserName, L'\\') != nullptr)
        {
            hr = SplitDomainAndUsername(_pszQualifiedUserName, &pwzDomain, &pwzUsername);
        }
        else if (_pwzUsername != nullptr && wcschr(_pwzUsername, L'\\') != nullptr)
        {
            hr = SplitDomainAndUsername(_pwzUsername, &pwzDomain, &pwzUsername);
        }
        else if (_pwzUsername != nullptr && wcschr(_pwzUsername, L'@') != nullptr)
        {
            hr = SHStrDupW(L"MicrosoftAccount", &pwzDomain);
            if (SUCCEEDED(hr))
            {
                hr = SHStrDupW(_pwzUsername, &pwzUsername);
            }
        }
        else
        {
            WCHAR wsz[MAX_COMPUTERNAME_LENGTH+1];
            DWORD cch = ARRAYSIZE(wsz);
            if (!GetComputerNameW(wsz, &cch)) {
                LogDebugMessage(L"[ERROR] GetComputerNameW失败");
                return E_FAIL;
            }

            hr = SHStrDupW(wsz, &pwzDomain);
            if (SUCCEEDED(hr) && _pwzUsername != nullptr)
            {
                hr = SHStrDupW(_pwzUsername, &pwzUsername);
            }
        }

        if (FAILED(hr) || pwzDomain == nullptr || pwzUsername == nullptr)
        {
            SAFE_COTASK_MEM_FREE(pwzDomain);
            SAFE_COTASK_MEM_FREE(pwzUsername);
            LogDebugMessage(L"[ERROR] 登录用户名拆分/构造失败: 0x%08X", hr);
            return FAILED(hr) ? hr : E_FAIL;
        }

        LogDebugMessage(L"[INFO] 序列化登录名 domain=%s username=%s", pwzDomain, pwzUsername);

        PCWSTR password = _rgFieldStrings[SFI_PASSWORD];
        if (useStoredSecret)
        {
            hr = RequestOneTimeLogonSecret(&pwzOneTimePassword);
            if (FAILED(hr))
            {
                LogDebugMessage(L"[ERROR] 获取一次性登录凭据失败: 0x%08X", hr);
                *pcpsiOptionalStatusIcon = CPSI_ERROR;
                SHStrDupW(L"已保存的登录凭据不可用，请使用 Windows 密码登录。", ppwszOptionalStatusText);
                {
                    std::lock_guard<std::mutex> lock(_faceMutex);
                    _fFaceCredentialReady = false;
                    _fHideCredentialInputFields = false;
                    _rgFieldStatePairs[SFI_PASSWORD] = {
                        CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_FOCUSED};
                    _rgFieldStatePairs[SFI_SUBMIT_BUTTON] = {
                        CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_NONE};
                }
                if (_pCredProvCredentialEvents)
                {
                    _pCredProvCredentialEvents->BeginFieldUpdates();
                    _pCredProvCredentialEvents->SetFieldState(
                        this, SFI_PASSWORD, CPFS_DISPLAY_IN_SELECTED_TILE);
                    _pCredProvCredentialEvents->SetFieldState(
                        this, SFI_SUBMIT_BUTTON, CPFS_DISPLAY_IN_SELECTED_TILE);
                    _pCredProvCredentialEvents->EndFieldUpdates();
                }
                SAFE_COTASK_MEM_FREE(pwzDomain);
                SAFE_COTASK_MEM_FREE(pwzUsername);
                return hr;
            }
            password = pwzOneTimePassword;
        }

        LogDebugMessage(L"[INFO] 使用KerbInteractiveUnlockLogon方式处理凭证");
        hr = ProtectIfNecessaryAndCopyPassword(password, _cpus, &pwzProtectedPassword);
        LogDebugMessage(L"[DEBUG] ProtectIfNecessaryAndCopyPassword返回: 0x%08X", hr);

        if (SUCCEEDED(hr))
        {
            KERB_INTERACTIVE_UNLOCK_LOGON kiul;

            hr = KerbInteractiveUnlockLogonInit(pwzDomain, pwzUsername, pwzProtectedPassword, _cpus, &kiul);
            LogDebugMessage(L"[DEBUG] KerbInteractiveUnlockLogonInit返回: 0x%08X, 使用用户名: %s, 密码: %s",
                           hr, pwzUsername ? pwzUsername : L"NULL", pwzProtectedPassword ? L"****" : L"NULL");

            if (SUCCEEDED(hr))
            {
                hr = KerbInteractiveUnlockLogonPack(kiul, &pcpcs->rgbSerialization, &pcpcs->cbSerialization);
                LogDebugMessage(L"[DEBUG] KerbInteractiveUnlockLogonPack返回: 0x%08X, 序列化大小: %d",
                               hr, pcpcs->cbSerialization);

                if (SUCCEEDED(hr))
                {
                    ULONG ulAuthPackage;
                    hr = RetrieveNegotiateAuthPackage(&ulAuthPackage);
                    LogDebugMessage(L"[DEBUG] RetrieveNegotiateAuthPackage返回: 0x%08X, AuthPackage: %u",
                                   hr, ulAuthPackage);

                    if (SUCCEEDED(hr))
                    {
                        pcpcs->ulAuthenticationPackage = ulAuthPackage;
                        pcpcs->clsidCredentialProvider = CLSID_CSample;
                        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
                        _fLastSerializationUsedStoredSecret = useStoredSecret;
                        LogDebugMessage(L"[INFO] GetSerialization成功！凭证已打包，状态设为CPGSR_RETURN_CREDENTIAL_FINISHED");
                    } else {
                        LogDebugMessage(L"[ERROR] RetrieveNegotiateAuthPackage失败");
                    }
                } else {
                    LogDebugMessage(L"[ERROR] KerbInteractiveUnlockLogonPack失败");
                }
            } else {
                LogDebugMessage(L"[ERROR] KerbInteractiveUnlockLogonInit失败");
            }
        } else {
            LogDebugMessage(L"[ERROR] ProtectIfNecessaryAndCopyPassword失败");
        }

        SecureFreePassword(pwzProtectedPassword);
        SecureFreePassword(pwzOneTimePassword);
        SAFE_COTASK_MEM_FREE(pwzDomain);
        SAFE_COTASK_MEM_FREE(pwzUsername);
    }
    
    if (FAILED(hr) && pcpcs->rgbSerialization != nullptr)
    {
        SecureZeroMemory(pcpcs->rgbSerialization, pcpcs->cbSerialization);
        CoTaskMemFree(pcpcs->rgbSerialization);
        pcpcs->rgbSerialization = nullptr;
        pcpcs->cbSerialization = 0;
    }
    LogDebugMessage(L"[INFO] GetSerialization返回: hr=0x%08X, pcpgsr=%d", hr, *pcpgsr);
    return hr;
}

struct REPORT_RESULT_STATUS_INFO
{
    NTSTATUS ntsStatus;
    NTSTATUS ntsSubstatus;
    PWSTR     pwzMessage;
    CREDENTIAL_PROVIDER_STATUS_ICON cpsi;
};

static const REPORT_RESULT_STATUS_INFO s_rgLogonStatusInfo[] =
{
    { (NTSTATUS)0xC000006E, (NTSTATUS)0x00000000, const_cast<PWSTR>(L"Incorrect password or username."), CPSI_ERROR, },
    { (NTSTATUS)0xC0000202, (NTSTATUS)0xC0000001, const_cast<PWSTR>(L"The account is disabled."), CPSI_WARNING },
};

// ReportResult is completely optional.  Its purpose is to allow a credential to customize the string
// and the icon displayed in the case of a logon failure.  For example, we have chosen to
// customize the error shown in the case of bad username/password and in the case of the account
// being disabled.
HRESULT CSampleCredential::ReportResult(NTSTATUS ntsStatus,
                                        NTSTATUS ntsSubstatus,
                                        _Outptr_result_maybenull_ PWSTR *ppwszOptionalStatusText,
                                        _Out_ CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon)
{
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    DWORD dwStatusInfo = (DWORD)-1;

    // Look for a match on status and substatus.
    for (DWORD i = 0; i < ARRAYSIZE(s_rgLogonStatusInfo); i++)
    {
        if (s_rgLogonStatusInfo[i].ntsStatus == ntsStatus && s_rgLogonStatusInfo[i].ntsSubstatus == ntsSubstatus)
        {
            dwStatusInfo = i;
            break;
        }
    }

    if ((DWORD)-1 != dwStatusInfo)
    {
        if (SUCCEEDED(SHStrDupW(s_rgLogonStatusInfo[dwStatusInfo].pwzMessage, ppwszOptionalStatusText)))
        {
            *pcpsiOptionalStatusIcon = s_rgLogonStatusInfo[dwStatusInfo].cpsi;
        }
    }

    const bool logonFailed = FAILED(HRESULT_FROM_NT(ntsStatus));
    if (_fLastSerializationUsedStoredSecret && logonFailed
        && _pszUserSid != nullptr && _lastSecretRequestId != 0)
    {
        const auto staleHr = LogonSecretClient{}.mark_stale(
            _pszUserSid, _lastSecretRequestId, _lastSecretSessionId);
        if (FAILED(staleHr))
        {
            LogDebugMessage(L"[ERROR] 标记已保存登录凭据失效失败: 0x%08X", staleHr);
        }
    }

    {
        std::lock_guard<std::mutex> lock(_faceMutex);
        _fFaceCredentialReady = false;
        _fHideCredentialInputFields = false;
        if (logonFailed)
        {
            _rgFieldStatePairs[SFI_PASSWORD] = {
                CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_FOCUSED};
            _rgFieldStatePairs[SFI_SUBMIT_BUTTON] = {
                CPFS_DISPLAY_IN_SELECTED_TILE, CPFIS_NONE};
        }
    }
    _fLastSerializationUsedStoredSecret = false;
    _lastSecretRequestId = 0;
    _lastSecretSessionId = 0;
    if (_pProvider)
    {
        _pProvider->ResetCredentialReady();
    }

    // Keep the normal password path available after any rejected submission.
    if (logonFailed)
    {
        const auto clearHr = ClearPasswordValue(_rgFieldStrings[SFI_PASSWORD]);
        _pwzPassword = _rgFieldStrings[SFI_PASSWORD];
        if (_pCredProvCredentialEvents)
        {
            _pCredProvCredentialEvents->BeginFieldUpdates();
            _pCredProvCredentialEvents->SetFieldState(this, SFI_PASSWORD, CPFS_DISPLAY_IN_SELECTED_TILE);
            _pCredProvCredentialEvents->SetFieldState(this, SFI_SUBMIT_BUTTON, CPFS_DISPLAY_IN_SELECTED_TILE);
            if (SUCCEEDED(clearHr))
            {
                _pCredProvCredentialEvents->SetFieldString(
                    this, SFI_PASSWORD, _rgFieldStrings[SFI_PASSWORD]);
            }
            _pCredProvCredentialEvents->EndFieldUpdates();
        }
    }

    // Since nullptr is a valid value for *ppwszOptionalStatusText and *pcpsiOptionalStatusIcon
    // this function can't fail.
    return S_OK;
}

// Gets the SID of the user corresponding to the credential.
HRESULT CSampleCredential::GetUserSid(_Outptr_result_nullonfailure_ PWSTR *ppszSid)
{
    *ppszSid = nullptr;
    HRESULT hr = S_FALSE;
    if (_pszUserSid != nullptr)
    {
        hr = SHStrDupW(_pszUserSid, ppszSid);
    }
    // Return S_FALSE with a null SID in ppszSid for the
    // credential to be associated with an empty user tile.

    return hr;
}

// GetFieldOptions to enable the password reveal button and touch keyboard auto-invoke in the password field.
HRESULT CSampleCredential::GetFieldOptions(DWORD dwFieldID,
                                           _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS *pcpcfo)
{
    *pcpcfo = CPCFO_NONE;

    if (dwFieldID == SFI_PASSWORD)
    {
        *pcpcfo = CPCFO_ENABLE_PASSWORD_REVEAL;
    }
    else if (dwFieldID == SFI_TILEIMAGE)
    {
        *pcpcfo = CPCFO_ENABLE_TOUCH_KEYBOARD_AUTO_INVOKE;
    }

    return S_OK;
}

// ============ 新增：改进的生命周期管理函数 ============

bool CSampleCredential::IsProcessStillRunning() {
    std::lock_guard<std::mutex> service_lock(_serviceMutex);

    if (_hSmile2UnlockProcess == nullptr) {
        return false;
    }

    DWORD dwExitCode;
    if (GetExitCodeProcess(_hSmile2UnlockProcess, &dwExitCode)) {
        return dwExitCode == STILL_ACTIVE;
    }

    return false;
}

HRESULT CSampleCredential::TerminateSmile2UnlockService() {
    std::lock_guard<std::mutex> service_lock(_serviceMutex);

    if (_hSmile2UnlockProcess == nullptr) {
        return S_OK;
    }

    // 检查进程是否仍在运行
    DWORD dwExitCode;
    if (GetExitCodeProcess(_hSmile2UnlockProcess, &dwExitCode)) {
        if (dwExitCode == STILL_ACTIVE) {
            LogDebugMessage(L"[INFO] Smile2Unlock服务进程仍在运行，尝试终止");

            DWORD waitResult = WaitForSingleObject(_hSmile2UnlockProcess, 1000);
            if (waitResult == WAIT_TIMEOUT) {
                LogDebugMessage(L"[WARNING] 服务进程未及时退出，尝试强制终止");
                
                // 尝试强制终止
                if (!TerminateProcess(_hSmile2UnlockProcess, 1)) {
                    DWORD dwError = GetLastError();
                    // ACCESS_DENIED 是常见的，可能是权限不足或进程已退出
                    if (dwError == ERROR_ACCESS_DENIED) {
                        LogDebugMessage(L"[INFO] 无法强制终止进程（权限不足或已退出），等待后清理");
                        // 等待短暂时间后检查进程是否已退出
                        WaitForSingleObject(_hSmile2UnlockProcess, 100);
                    } else {
                        LogDebugMessage(L"[WARNING] 无法强制终止进程，错误代码: 0x%08X", dwError);
                    }
                } else {
                    LogDebugMessage(L"[INFO] Smile2Unlock 服务进程已被强制终止");
                    // 等待进程完全退出
                    WaitForSingleObject(_hSmile2UnlockProcess, 100);
                }
            } else if (waitResult == WAIT_OBJECT_0) {
                LogDebugMessage(L"[INFO] Smile2Unlock服务进程已退出");
            } else if (waitResult == WAIT_FAILED) {
                DWORD dwError = GetLastError();
                LogDebugMessage(L"[WARNING] 等待进程退出失败，错误代码: 0x%08X", dwError);
            }
        } else {
            LogDebugMessage(L"[INFO] Smile2Unlock服务进程已退出，退出代码: %d", dwExitCode);
        }
    } else {
        DWORD dwError = GetLastError();
        LogDebugMessage(L"[WARNING] 获取进程退出代码失败，错误代码: 0x%08X", dwError);
    }

    // 清理进程句柄
    if (_hSmile2UnlockProcess != nullptr) {
        CloseHandle(_hSmile2UnlockProcess);
        _hSmile2UnlockProcess = nullptr;
        _dwSmile2UnlockPID = 0;
        LogDebugMessage(L"[INFO] 已清理Smile2Unlock进程句柄");
    }

    return S_OK;
}

HRESULT CSampleCredential::StartFaceRecognitionAsync() {
    if (_pProvider) {
        _pProvider->ResetCredentialReady();
    }

    LogDebugMessage(L"[INFO] StartFaceRecognitionAsync - thread_joinable=%d process_alive=%d",
                    _faceRecognitionThread.joinable() ? 1 : 0,
                    (_hSmile2UnlockProcess != nullptr && IsProcessStillRunning()) ? 1 : 0);

    {
        std::lock_guard<std::mutex> lock(_faceMutex);
        if (_fFaceRecognitionRunning) {
            const bool has_running_thread = _faceRecognitionThread.joinable();
            const bool has_running_process = (_hSmile2UnlockProcess != nullptr) && IsProcessStillRunning();
            if (has_running_thread || has_running_process) {
                LogDebugMessage(L"[INFO] 人脸识别已在运行，忽略重复请求");
                return S_OK;
            }

            LogDebugMessage(L"[WARNING] 检测到人脸识别运行标记残留，正在清理陈旧状态");
            _fFaceRecognitionRunning = false;
        }
    }

    // 启动识别线程
    try {
        // 使用现代C++特性：带超时的异步线程停止
        if (_faceRecognitionThread.joinable()) {
            LogDebugMessage(L"[INFO] 检测到旧线程仍在运行，尝试停止...");
            
            // 先设置取消标志
            {
                std::lock_guard<std::mutex> lock(_faceMutex);
                _fFaceRecognitionRunning = false;
            }
            
            // 使用 std::async 和 std::future 实现异步 join
            auto stop_future = std::async(std::launch::async, [this]() {
                try {
                    _faceRecognitionThread.join();
                    LogDebugMessage(L"[INFO] 旧线程已成功停止");
                } catch (const std::exception& e) {
                    LogDebugMessage(L"[WARNING] 停止旧线程时发生异常");
                }
            });
            
            // 等待线程完成（无限等待）
            auto status = stop_future.wait_for(std::chrono::minutes(5));
            if (status == std::future_status::timeout) {
                LogDebugMessage(L"[WARNING] 旧线程未在 5 分钟内停止，分离线程以避免阻塞");
                _faceRecognitionThread.detach();  // 分离线程，让它在后台运行
            }
        }

        // 重置识别状态
        extern std::atomic<RecognitionStatus> face_recognition_status;
        face_recognition_status = RecognitionStatus::IDLE;

        // 设置运行标志
        {
            std::lock_guard<std::mutex> lock(_faceMutex);
            _fFaceRecognitionRunning = true;
        }

        const ULONGLONG session_generation = _serviceGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        LogDebugMessage(L"[INFO] 创建新的人脸识别会话，generation=%llu", session_generation);

        // 启动新线程
        _faceRecognitionThread = std::thread([this, session_generation]() {
            auto is_session_current = [this, session_generation]() -> bool {
                if (_serviceGeneration.load(std::memory_order_acquire) != session_generation) {
                    return false;
                }

                std::lock_guard<std::mutex> lock(_faceMutex);
                return _fFaceRecognitionRunning;
            };

            LogDebugMessage(L"[INFO] 启动人脸识别线程，generation=%llu", session_generation);

            LogDebugMessage(L"[INFO] 启动 Smile2Unlock 服务并请求识别");
            HRESULT hr = LaunchSmile2UnlockService();
            if (SUCCEEDED(hr)) {
                const ULONGLONG current_generation = _serviceGeneration.load(std::memory_order_acquire);
                if (current_generation != session_generation) {
                    LogDebugMessage(L"[WARNING] 检测到识别会话已被新的 generation=%llu 替换，取消本次启动 generation=%llu",
                                    current_generation, session_generation);
                    hr = E_ABORT;
                }
            }

            if (SUCCEEDED(hr) && is_session_current()) {
                hr = SendAuthRequestToSmile2Unlock(AuthRequestType::START_RECOGNITION);
            }

            if (SUCCEEDED(hr) && is_session_current()) {
                HRESULT waitResult = WaitForFaceRecognitionResult();

                if (SUCCEEDED(waitResult) && is_session_current()) {
                    LogDebugMessage(L"[INFO] 人脸识别成功，等待一次性凭证序列化");
                    {
                        std::lock_guard<std::mutex> lock(_faceMutex);
                        _fFaceCredentialReady = true;
                        _fHideCredentialInputFields = true;
                        _rgFieldStatePairs[SFI_EDIT_TEXT] = { CPFS_HIDDEN, CPFIS_NONE };
                        _rgFieldStatePairs[SFI_PASSWORD] = { CPFS_HIDDEN, CPFIS_NONE };
                        _rgFieldStatePairs[SFI_SUBMIT_BUTTON] = { CPFS_HIDDEN, CPFIS_NONE };
                    }
                    if (_pCredProvCredentialEvents) {
                        _pCredProvCredentialEvents->BeginFieldUpdates();
                        _pCredProvCredentialEvents->SetFieldState(this, SFI_EDIT_TEXT, CPFS_HIDDEN);
                        _pCredProvCredentialEvents->SetFieldState(this, SFI_PASSWORD, CPFS_HIDDEN);
                        _pCredProvCredentialEvents->SetFieldState(this, SFI_SUBMIT_BUTTON, CPFS_HIDDEN);
                        _pCredProvCredentialEvents->SetFieldString(
                            this, SFI_LARGE_TEXT, L"人脸识别成功，正在登录...");
                        _pCredProvCredentialEvents->EndFieldUpdates();
                        if (_pProvider) {
                            _pProvider->OnCredentialReady();
                        }
                    }
                } else if (FAILED(waitResult) && is_session_current()) {
                    LogDebugMessage(L"[ERROR] 人脸识别失败或超时");
                    if (_pCredProvCredentialEvents) {
                        _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT, L"人脸识别失败");
                    }
                } else {
                    LogDebugMessage(L"[INFO] 人脸识别线程检测到会话已过期，跳过结果回写，generation=%llu",
                                    session_generation);
                }
            } else {
                LogDebugMessage(L"[ERROR] 启动Smile2Unlock服务或发送识别请求失败: 0x%08X", hr);
            }

            const ULONGLONG current_generation = _serviceGeneration.load(std::memory_order_acquire);
            bool has_owned_process = false;
            {
                std::lock_guard<std::mutex> service_lock(_serviceMutex);
                has_owned_process = (_hSmile2UnlockProcess != nullptr);
            }

            if (current_generation == session_generation && has_owned_process) {
                LogDebugMessage(L"[INFO] 人脸识别线程准备清理当前启动的 Smile2Unlock 服务，generation=%llu", session_generation);
                TerminateSmile2UnlockService();
            } else if (has_owned_process) {
                LogDebugMessage(L"[INFO] 跳过过期识别线程的服务清理：current_generation=%llu, session_generation=%llu",
                                current_generation, session_generation);
            }

            // 清理
            {
                std::lock_guard<std::mutex> lock(_faceMutex);
                _fFaceRecognitionRunning = false;
            }
            LogDebugMessage(L"[INFO] 人脸识别线程结束，generation=%llu", session_generation);
        });

        return S_OK;
    } catch (const std::exception& e) {
        LogDebugMessage(L"[ERROR] 启动识别线程异常");
        return E_FAIL;
    }
}

void CSampleCredential::StopFaceRecognition() {
    LogDebugMessage(L"[INFO] 停止人脸识别");

    // 先设置取消标志，让识别线程知道应该停止
    {
        std::lock_guard<std::mutex> lock(_faceMutex);
        _fFaceRecognitionRunning = false;
    }

    const ULONGLONG stop_generation = _serviceGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    LogDebugMessage(L"[INFO] 开始异步停止 Smile2Unlock 服务，generation=%llu", stop_generation);

    AddRef();
    std::thread([this, stop_generation]() {
        struct ReleaseGuard {
            CSampleCredential* credential;
            ~ReleaseGuard() {
                if (credential != nullptr) {
                    credential->Release();
                }
            }
        } release_guard{this};

        const ULONGLONG current_generation_before_cancel = _serviceGeneration.load(std::memory_order_acquire);
        if (current_generation_before_cancel != stop_generation) {
            LogDebugMessage(L"[INFO] 跳过过期停止任务：current_generation=%llu, stop_generation=%llu",
                            current_generation_before_cancel, stop_generation);
            return;
        }

        SendAuthRequestToSmile2Unlock(AuthRequestType::CANCEL_RECOGNITION);

        const ULONGLONG current_generation_before_terminate = _serviceGeneration.load(std::memory_order_acquire);
        if (current_generation_before_terminate != stop_generation) {
            LogDebugMessage(L"[INFO] 停止任务在终止服务前已过期：current_generation=%llu, stop_generation=%llu",
                            current_generation_before_terminate, stop_generation);
            return;
        }

        TerminateSmile2UnlockService();
        LogDebugMessage(L"[INFO] Smile2Unlock 服务停止完成，generation=%llu", stop_generation);
    }).detach();

    LogDebugMessage(L"[INFO] 人脸识别停止信号已发送，将在后台完成清理");
}
