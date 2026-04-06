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
#include <wincrypt.h>

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
#include "models/udp_password_packet.h"
#include "registryhelper.h"
#include "CSampleProvider.h"
#include "hresult_helper.h"
#include <chrono>
#include <cwchar>
#include <mutex>
#include <thread>
#include <future>
#include <stdio.h>
#include <stdarg.h>
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
    case AuthRequestType::GET_PASSWORD:
      return false;
    default:
      return false;
  }
}

bool Utf8ToWidePassword(const std::string& utf8_password, PWSTR* ppwszPassword) {
  *ppwszPassword = nullptr;

  const int bufferSize = MultiByteToWideChar(CP_UTF8, 0, utf8_password.c_str(), -1, nullptr, 0);
  if (bufferSize <= 0) {
    return false;
  }

  PWSTR password = static_cast<PWSTR>(CoTaskMemAlloc(bufferSize * sizeof(wchar_t)));
  if (!password) {
    return false;
  }

  if (!MultiByteToWideChar(CP_UTF8, 0, utf8_password.c_str(), -1, password, bufferSize)) {
    CoTaskMemFree(password);
    return false;
  }

  *ppwszPassword = password;
  return true;
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
                                      _In_ ICredentialProviderUser *pcpUser,
                                      _In_opt_ CSampleProvider *pProvider)
{
    HRESULT hr = S_OK;
    LogDebugMessage(L"[INFO] CSampleCredential::Initialize开始，使用场景: %d", cpus);
    _cpus = cpus;
    _pProvider = pProvider;  // 保存Provider指针

    GUID guidProvider;
    pcpUser->GetProviderID(&guidProvider);
    _fIsLocalUser = (guidProvider == Identity_LocalUserProvider);
    LogDebugMessage(L"[INFO] 用户类型: %s", _fIsLocalUser ? L"本地用户" : L"域用户");

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
    
    INIT_FIELD_STRING(SFI_LABEL, L"Sample Credential", L"SFI_LABEL");
    INIT_FIELD_STRING(SFI_LARGE_TEXT, L"Sample Credential Provider", L"SFI_LARGE_TEXT");
    INIT_FIELD_STRING(SFI_EDIT_TEXT, L"Edit Text", L"SFI_EDIT_TEXT");
    INIT_FIELD_STRING(SFI_PASSWORD, L"", L"SFI_PASSWORD");
    INIT_FIELD_STRING(SFI_SUBMIT_BUTTON, L"Submit", L"SFI_SUBMIT_BUTTON");
    INIT_FIELD_STRING(SFI_CHECKBOX, L"Checkbox", L"SFI_CHECKBOX");
    INIT_FIELD_STRING(SFI_COMBOBOX, L"Combobox", L"SFI_COMBOBOX");
    INIT_FIELD_STRING(SFI_LAUNCHWINDOW_LINK, L"Launch helper window", L"SFI_LAUNCHWINDOW_LINK");
    INIT_FIELD_STRING(SFI_HIDECONTROLS_LINK, L"Hide additional controls", L"SFI_HIDECONTROLS_LINK");
    
    #undef INIT_FIELD_STRING
    
    // 获取用户信息
    HRESULT hrTemp = pcpUser->GetStringValue(PKEY_Identity_QualifiedUserName, &_pszQualifiedUserName);
    if (FAILED(hrTemp)) {
        LogDebugMessage(L"[WARNING] Get QualifiedUserName failed: 0x%08X", hrTemp);
        if (SUCCEEDED(hr)) hr = hrTemp;
        _pszQualifiedUserName = nullptr;
    }

    // 获取用户名
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
        SAFE_COTASK_MEM_FREE(pszUserName); // 安全释放，即使为nullptr
    }

    // 获取显示名
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

    // 获取登录状态
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

    // 获取用户SID
    hrTemp = pcpUser->GetSid(&_pszUserSid);
    if (FAILED(hrTemp)) {
        LogDebugMessage(L"[WARNING] Get User SID failed: 0x%08X", hrTemp);
        if (SUCCEEDED(hr)) hr = hrTemp;
        _pszUserSid = nullptr;
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
  std::lock_guard<std::mutex> service_lock(_serviceMutex);

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

  // 刚开机时 SU 冷启动较慢，给它一个短暂的自检/绑定窗口，避免过早发送首个 START 请求。
  constexpr DWORD kStartupGraceMs = 800;
  constexpr DWORD kStartupPollMs = 50;
  for (DWORD waited = 0; waited < kStartupGraceMs; waited += kStartupPollMs) {
    DWORD dwExitCode = STILL_ACTIVE;
    if (!GetExitCodeProcess(_hSmile2UnlockProcess, &dwExitCode)) {
      break;
    }

    if (dwExitCode != STILL_ACTIVE) {
      LogDebugMessage(L"[ERROR] Smile2Unlock 在冷启动等待阶段提前退出，退出代码: %d", dwExitCode);
      CloseHandle(_hSmile2UnlockProcess);
      _hSmile2UnlockProcess = nullptr;
      _dwSmile2UnlockPID = 0;
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

HRESULT CSampleCredential::RequestAndDecryptPasswordFromSU(PWSTR *ppwszPassword) {
  *ppwszPassword = nullptr;

  WSADATA wsaData{};
  const int wsaStartupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (wsaStartupResult != 0) {
    LogDebugMessage(L"[ERROR] WSAStartup 失败: %d", wsaStartupResult);
    return E_FAIL;
  }

  HRESULT hr = E_FAIL;
  SOCKET sock = INVALID_SOCKET;

  do {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
      LogDebugMessage(L"[ERROR] 创建 UDP socket 失败: %d", WSAGetLastError());
      break;
    }

    DWORD timeout_ms = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local_addr.sin_port = htons(51237);

    if (bind(sock, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) == SOCKET_ERROR) {
      LogDebugMessage(L"[ERROR] 绑定密码响应端口失败: %d", WSAGetLastError());
      break;
    }

    extern std::string recognized_username;
    std::string username_hint = recognized_username;
    if (username_hint.empty() && _pwzUsername != nullptr) {
      int required = WideCharToMultiByte(CP_UTF8, 0, _pwzUsername, -1, nullptr, 0, nullptr, nullptr);
      if (required > 1) {
        username_hint.resize(static_cast<size_t>(required));
        WideCharToMultiByte(CP_UTF8, 0, _pwzUsername, -1, username_hint.data(), required, nullptr, nullptr);
        username_hint.resize(static_cast<size_t>(required - 1));
      }
    }

    if (username_hint.empty()) {
      LogDebugMessage(L"[ERROR] 无法确定请求密码的用户名");
      break;
    }

    const uint32_t session_id = GetTickCount();
    AuthRequestSender sender("127.0.0.1", 51236);
    if (!sender.send_request(AuthRequestType::GET_PASSWORD, username_hint, session_id)) {
      LogDebugMessage(L"[ERROR] 发送 GET_PASSWORD 请求失败");
      break;
    }

    LogDebugMessage(L"[INFO] 已向 SU 请求密码，用户名: %hs, session=%u", username_hint.c_str(), session_id);

    UdpPasswordResponsePacket response{};
    sockaddr_in remote_addr{};
    int remote_addr_len = sizeof(remote_addr);
    const int received = recvfrom(sock,
                                  reinterpret_cast<char*>(&response),
                                  sizeof(response),
                                  0,
                                  reinterpret_cast<sockaddr*>(&remote_addr),
                                  &remote_addr_len);
    if (received == SOCKET_ERROR) {
      LogDebugMessage(L"[ERROR] 接收密码响应失败: %d", WSAGetLastError());
      break;
    }

    if (static_cast<size_t>(received) < sizeof(UdpPasswordResponsePacket)) {
      LogDebugMessage(L"[ERROR] 密码响应包长度非法: %d", received);
      break;
    }
    if (response.magic_number != PASSWORD_RESPONSE_MAGIC ||
        response.version != PASSWORD_RESPONSE_VERSION ||
        response.session_id != session_id) {
      LogDebugMessage(L"[ERROR] 密码响应包校验失败");
      break;
    }
    if (response.result_code != 0) {
      LogDebugMessage(L"[ERROR] SU 返回密码失败，result=%d", response.result_code);
      break;
    }
    if (response.protected_password_size == 0 ||
        response.protected_password_size > sizeof(response.protected_password)) {
      LogDebugMessage(L"[ERROR] 受保护密码长度非法: %u", response.protected_password_size);
      break;
    }

    DATA_BLOB input_blob{};
    input_blob.pbData = response.protected_password;
    input_blob.cbData = response.protected_password_size;

    DATA_BLOB output_blob{};
    if (!CryptUnprotectData(&input_blob, nullptr, nullptr, nullptr, nullptr, 0, &output_blob)) {
      LogDebugMessage(L"[ERROR] CryptUnprotectData 失败: %u", GetLastError());
      break;
    }

    std::string decrypted_password(reinterpret_cast<const char*>(output_blob.pbData), output_blob.cbData);
    if (!Utf8ToWidePassword(decrypted_password, ppwszPassword)) {
      SecureZeroMemory(decrypted_password.data(), decrypted_password.size());
      SecureZeroMemory(output_blob.pbData, output_blob.cbData);
      LocalFree(output_blob.pbData);
      LogDebugMessage(L"[ERROR] 密码 UTF-8 转宽字符失败");
      hr = E_FAIL;
      break;
    }

    SecureZeroMemory(decrypted_password.data(), decrypted_password.size());
    SecureZeroMemory(output_blob.pbData, output_blob.cbData);
    LocalFree(output_blob.pbData);

    LogDebugMessage(L"[INFO] 已从 SU 获取并解密密码");
    LogDebugMessage(L"[INFO] 密码解密结果长度=%u", static_cast<unsigned>(wcslen(*ppwszPassword)));
    hr = S_OK;
  } while (false);

  if (sock != INVALID_SOCKET) {
    closesocket(sock);
  }
  WSACleanup();
  return hr;
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

    LogDebugMessage(L"[INFO] GetSerialization被调用");
    LogDebugMessage(L"[DEBUG] _pwzUsername: %s", _pwzUsername ? _pwzUsername : L"NULL");
    LogDebugMessage(L"[DEBUG] 密码字段内容: %s", _rgFieldStrings[SFI_PASSWORD] ? L"已填写" : L"为空");
    LogDebugMessage(L"[DEBUG] 密码字段长度: %u", _rgFieldStrings[SFI_PASSWORD] ? static_cast<unsigned>(wcslen(_rgFieldStrings[SFI_PASSWORD])) : 0);
    LogDebugMessage(L"[DEBUG] QualifiedUserName: %s", _pszQualifiedUserName ? _pszQualifiedUserName : L"NULL");

    // 获取计算机名称作为域
    WCHAR wsz[MAX_COMPUTERNAME_LENGTH+1];
    DWORD cch = ARRAYSIZE(wsz);
    if (!GetComputerNameW(wsz, &cch)) {
        LogDebugMessage(L"[ERROR] GetComputerNameW失败");
        return E_FAIL;
    }
    
    LogDebugMessage(L"[INFO] 计算机名称: %s", wsz);
    
    // 设置 _pwzPassword 为当前密码字段值（来自Sparkin的方式）
    _pwzPassword = _rgFieldStrings[SFI_PASSWORD];
    
    // 对所有用户类型使用统一的 KerbInteractiveUnlockLogon 方式
    LogDebugMessage(L"[INFO] 使用KerbInteractiveUnlockLogon方式处理凭证");
    PWSTR pwzProtectedPassword;
    hr = ProtectIfNecessaryAndCopyPassword(_rgFieldStrings[SFI_PASSWORD], _cpus, &pwzProtectedPassword);
    LogDebugMessage(L"[DEBUG] ProtectIfNecessaryAndCopyPassword返回: 0x%08X", hr);
    
    if (SUCCEEDED(hr))
    {
        KERB_INTERACTIVE_UNLOCK_LOGON kiul;
        
        // 关键：使用原始用户名（格式: domain\username 或 MicrosoftAccount\email）
        // 这里直接使用原始密码而非受保护的密码
        hr = KerbInteractiveUnlockLogonInit(wsz, _pwzUsername, _pwzPassword, _cpus, &kiul);
        LogDebugMessage(L"[DEBUG] KerbInteractiveUnlockLogonInit返回: 0x%08X, 使用用户名: %s, 密码: %s", 
                       hr, _pwzUsername ? _pwzUsername : L"NULL", _pwzPassword ? L"****" : L"NULL");
        
        if (SUCCEEDED(hr))
        {
            // We use KERB_INTERACTIVE_UNLOCK_LOGON in both unlock and logon scenarios.  It contains a
            // KERB_INTERACTIVE_LOGON to hold the creds plus a LUID that is filled in for us by Winlogon
            // as necessary.
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
                    // At this point the credential has created the serialized credential used for logon
                    // By setting this to CPGSR_RETURN_CREDENTIAL_FINISHED we are letting logonUI know
                    // that we have all the information we need and it should attempt to submit the
                    // serialized credential.
                    *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
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
        CoTaskMemFree(pwzProtectedPassword);
    } else {
        LogDebugMessage(L"[ERROR] ProtectIfNecessaryAndCopyPassword失败");
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

    // If we failed the logon, try to erase the password field.
    if (FAILED(HRESULT_FROM_NT(ntsStatus)))
    {
        if (_pCredProvCredentialEvents)
        {
            _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, L"");
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
    HRESULT hr = E_UNEXPECTED;
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

            if (SUCCEEDED(hr)) {
                hr = SendAuthRequestToSmile2Unlock(AuthRequestType::START_RECOGNITION);
            }

            if (SUCCEEDED(hr)) {
                HRESULT waitResult = WaitForFaceRecognitionResult();

                if (SUCCEEDED(waitResult)) {
                    LogDebugMessage(L"[INFO] 人脸识别成功，开始向 SU 请求密码");

                    // 识别成功，请求 SU 返回受保护密码并在本地解密
                    PWSTR pwszPassword = nullptr;
                    HRESULT decryptResult = RequestAndDecryptPasswordFromSU(&pwszPassword);

                    if (SUCCEEDED(decryptResult) && pwszPassword) {
                        LogDebugMessage(L"[INFO] 密码解密成功，设置密码字段");

                        // 设置密码到密码字段
                        {
                            std::lock_guard<std::mutex> lock(_faceMutex);
                            if (_rgFieldStrings[SFI_PASSWORD]) {
                                CoTaskMemFree(_rgFieldStrings[SFI_PASSWORD]);
                            }
                            _rgFieldStrings[SFI_PASSWORD] = pwszPassword;
                        }

                        // 更新UI
                        if (_pCredProvCredentialEvents) {
                            LogDebugMessage(L"[INFO] 更新CP字段并准备触发自动登录，events=%p provider=%p",
                                            _pCredProvCredentialEvents, _pProvider);
                            _pCredProvCredentialEvents->BeginFieldUpdates();
                            _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, pwszPassword);
                            _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT, L"人脸识别成功，正在登录...");
                            _pCredProvCredentialEvents->EndFieldUpdates();

                            // 通知Provider凭证已准备好
                            if (_pProvider) {
                                LogDebugMessage(L"[INFO] 通知Provider凭证已准备好");
                                _pProvider->OnCredentialReady();
                            }
                        } else {
                            LogDebugMessage(L"[ERROR] 识别成功但 _pCredProvCredentialEvents 为空，无法通知LogonUI更新字段");
                        }
                    } else {
                        LogDebugMessage(L"[ERROR] 密码解密失败: 0x%08X", decryptResult);
                        if (_pCredProvCredentialEvents) {
                            _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT, L"密码解密失败");
                        }
                    }
                } else {
                    LogDebugMessage(L"[ERROR] 人脸识别失败或超时");
                    if (_pCredProvCredentialEvents) {
                        _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT, L"人脸识别失败");
                    }
                }
            } else {
                LogDebugMessage(L"[ERROR] 启动Smile2Unlock服务或发送识别请求失败: 0x%08X", hr);
            }

            if (_hSmile2UnlockProcess != nullptr) {
                LogDebugMessage(L"[INFO] 人脸识别线程准备清理当前启动的 Smile2Unlock 服务");
                TerminateSmile2UnlockService();
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
    LogDebugMessage(L"[INFO] 开始同步停止 Smile2Unlock 服务，generation=%llu", stop_generation);

    SendAuthRequestToSmile2Unlock(AuthRequestType::CANCEL_RECOGNITION);

    // 使用带超时的异步线程停止
    if (_faceRecognitionThread.joinable()) {
        LogDebugMessage(L"[INFO] 尝试停止识别线程（超时2秒）");
        
        auto stop_future = std::async(std::launch::async, [this]() {
            try {
                _faceRecognitionThread.join();
                LogDebugMessage(L"[INFO] 识别线程已成功停止");
                return true;
            } catch (const std::exception& e) {
                LogDebugMessage(L"[WARNING] 停止识别线程时发生异常");
                return false;
            }
        });
        
        // 等待线程完成（无限等待）
        auto status = stop_future.wait_for(std::chrono::minutes(5));
        if (status == std::future_status::timeout) {
            LogDebugMessage(L"[WARNING] 识别线程未在 5 分钟内停止，分离线程以避免阻塞");
            _faceRecognitionThread.detach();  // 分离线程，让它在后台运行
        } else {
            // 线程已停止，获取结果
            bool stopped = stop_future.get();
            if (!stopped) {
                LogDebugMessage(L"[WARNING] 识别线程停止过程中出现问题");
            }
        }
    }

    TerminateSmile2UnlockService();
    LogDebugMessage(L"[INFO] Smile2Unlock 服务停止完成，generation=%llu", stop_generation);

    LogDebugMessage(L"[INFO] 人脸识别停止信号已发送，清理已完成");
}
