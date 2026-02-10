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
#include "Crypto.h"
#include "managers/ipc/udp/udp_receiver.h"
#include "registryhelper.h"
#include "CSampleProvider.h"
#include <chrono>
#include <cwchar>
#include <mutex>
#include <thread>
#include <future>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>

#define AUTO_RECOGNIZE_SUCCESS 0

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

  // 只在 AUTO_RECOGNIZE_SUCCESS 为 1 时输出到文件
#if AUTO_RECOGNIZE_SUCCESS
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
    _hFaceRecognizerProcess(nullptr),
    _dwFaceRecognizerPID(0),
    _fFaceRecognitionRunning(false),
    _fAutoStartEnabled(false),
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
    if (_hFaceRecognizerProcess) {
        CloseHandle(_hFaceRecognizerProcess);
        _hFaceRecognizerProcess = nullptr;
    }

    if (_rgFieldStrings[SFI_PASSWORD])
    {
        size_t lenPassword = wcslen(_rgFieldStrings[SFI_PASSWORD]);
        SecureZeroMemory(_rgFieldStrings[SFI_PASSWORD], lenPassword * sizeof(*_rgFieldStrings[SFI_PASSWORD]));
    }
    for (int i = 0; i < ARRAYSIZE(_rgFieldStrings); i++)
    {
        CoTaskMemFree(_rgFieldStrings[i]);
        CoTaskMemFree(_rgCredProvFieldDescriptors[i].pszLabel);
    }
    CoTaskMemFree(_pszUserSid);
    CoTaskMemFree(_pszQualifiedUserName);
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
    _cpus = cpus;
    _pProvider = pProvider;  // 保存Provider指针

    GUID guidProvider;
    pcpUser->GetProviderID(&guidProvider);
    _fIsLocalUser = (guidProvider == Identity_LocalUserProvider);

    // Copy the field descriptors for each field. This is useful if you want to vary the field
    // descriptors based on what Usage scenario the credential was created for.
    for (DWORD i = 0; SUCCEEDED(hr) && i < ARRAYSIZE(_rgCredProvFieldDescriptors); i++)
    {
        _rgFieldStatePairs[i] = rgfsp[i];
        hr = FieldDescriptorCopy(rgcpfd[i], &_rgCredProvFieldDescriptors[i]);
    }

    // Initialize the String value of all the fields.
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Sample Credential", &_rgFieldStrings[SFI_LABEL]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Sample Credential Provider", &_rgFieldStrings[SFI_LARGE_TEXT]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Edit Text", &_rgFieldStrings[SFI_EDIT_TEXT]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"", &_rgFieldStrings[SFI_PASSWORD]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Submit", &_rgFieldStrings[SFI_SUBMIT_BUTTON]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Checkbox", &_rgFieldStrings[SFI_CHECKBOX]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Combobox", &_rgFieldStrings[SFI_COMBOBOX]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Launch helper window", &_rgFieldStrings[SFI_LAUNCHWINDOW_LINK]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"Hide additional controls", &_rgFieldStrings[SFI_HIDECONTROLS_LINK]);
    }
    if (SUCCEEDED(hr))
    {
        hr = pcpUser->GetStringValue(PKEY_Identity_QualifiedUserName, &_pszQualifiedUserName);
    }
    if (SUCCEEDED(hr))
    {
        PWSTR pszUserName;
        pcpUser->GetStringValue(PKEY_Identity_UserName, &pszUserName);
        if (pszUserName != nullptr)
        {
            wchar_t szString[256];
            StringCchPrintf(szString, ARRAYSIZE(szString), L"User Name: %s", pszUserName);
            hr = SHStrDupW(szString, &_rgFieldStrings[SFI_FULLNAME_TEXT]);
            CoTaskMemFree(pszUserName);
        }
        else
        {
            hr =  SHStrDupW(L"User Name is NULL", &_rgFieldStrings[SFI_FULLNAME_TEXT]);
        }
    }
    if (SUCCEEDED(hr))
    {
        PWSTR pszDisplayName;
        pcpUser->GetStringValue(PKEY_Identity_DisplayName, &pszDisplayName);
        if (pszDisplayName != nullptr)
        {
            wchar_t szString[256];
            StringCchPrintf(szString, ARRAYSIZE(szString), L"Display Name: %s", pszDisplayName);
            hr = SHStrDupW(szString, &_rgFieldStrings[SFI_DISPLAYNAME_TEXT]);
            CoTaskMemFree(pszDisplayName);
        }
        else
        {
            hr = SHStrDupW(L"Display Name is NULL", &_rgFieldStrings[SFI_DISPLAYNAME_TEXT]);
        }
    }
    if (SUCCEEDED(hr))
    {
        PWSTR pszLogonStatus;
        pcpUser->GetStringValue(PKEY_Identity_LogonStatusString, &pszLogonStatus);
        if (pszLogonStatus != nullptr)
        {
            wchar_t szString[256];
            StringCchPrintf(szString, ARRAYSIZE(szString), L"Logon Status: %s", pszLogonStatus);
            hr = SHStrDupW(szString, &_rgFieldStrings[SFI_LOGONSTATUS_TEXT]);
            CoTaskMemFree(pszLogonStatus);
        }
        else
        {
            hr = SHStrDupW(L"Logon Status is NULL", &_rgFieldStrings[SFI_LOGONSTATUS_TEXT]);
        }
    }

    if (SUCCEEDED(hr))
    {
        hr = pcpUser->GetSid(&_pszUserSid);
    }

    // 初始化 _pwzUsername 和 _pwzPassword（来自 Sparkin 实现）
    if (SUCCEEDED(hr) && _pszQualifiedUserName)
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

    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"使用面部识别登录", &_rgFieldStrings[SFI_FACE_RECOGNITION_LINK]);
    }

    // 从注册表读取人脸识别配置
    if (SUCCEEDED(hr)) {
        _fAutoStartEnabled = RegistryHelper::ReadDwordFromRegistry(
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\auto_start", 0) != 0;
        _fWarmupModeEnabled = RegistryHelper::ReadDwordFromRegistry(
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\warmup_mode", 0) != 0;
        LogDebugMessage(L"[INFO] 从注册表读取配置: auto_start=%d, warmup_mode=%d",
            _fAutoStartEnabled, _fWarmupModeEnabled);
    }

    return hr;
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
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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

    // 如果启用了自动启动，立即启动人脸识别
    if (_fAutoStartEnabled) {
        // 检查是否已在运行
        bool alreadyRunning = false;
        {
            std::lock_guard<std::mutex> lock(_faceMutex);
            alreadyRunning = _fFaceRecognitionRunning;
        }

        if (!alreadyRunning) {
            LogDebugMessage(L"[INFO] 自动启动人脸识别已启用，开始异步识别");
            StartFaceRecognitionAsync();
        } else {
            LogDebugMessage(L"[INFO] 自动启动跳过：识别已在运行");
        }
    }

    return S_OK;
}

// Similarly to SetSelected, LogonUI calls this when your tile was selected
// and now no longer is. The most common thing to do here (which we do below)
// is to clear out the password field.
HRESULT CSampleCredential::SetDeselected()
{
    // 停止人脸识别
    StopFaceRecognition();

    HRESULT hr = S_OK;
    if (_rgFieldStrings[SFI_PASSWORD])
    {
        size_t lenPassword = wcslen(_rgFieldStrings[SFI_PASSWORD]);
        SecureZeroMemory(_rgFieldStrings[SFI_PASSWORD], lenPassword * sizeof(*_rgFieldStrings[SFI_PASSWORD]));

        CoTaskMemFree(_rgFieldStrings[SFI_PASSWORD]);
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
    HRESULT hr;

    // Validate parameters.
    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_EDIT_TEXT == _rgCredProvFieldDescriptors[dwFieldID].cpft ||
        CPFT_PASSWORD_TEXT == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        PWSTR *ppwszStored = &_rgFieldStrings[dwFieldID];
        CoTaskMemFree(*ppwszStored);
        hr = SHStrDupW(pwz, ppwszStored);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
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

HRESULT CSampleCredential::LaunchFaceRecognizer(const wchar_t* pszMode /* = L"recognize" */) {
  // 1. 检查现有进程是否仍在运行
  if (_hFaceRecognizerProcess != nullptr) {
    DWORD dwExitCode;
    if (GetExitCodeProcess(_hFaceRecognizerProcess, &dwExitCode)) {
      if (dwExitCode == STILL_ACTIVE) {
        LogDebugMessage(L"[INFO] FaceRecognizer进程仍在运行 (PID: %u)，复用现有进程", _dwFaceRecognizerPID);
        return S_OK; // 复用现有进程
      }
    }
    // 进程已结束，清理旧句柄
    LogDebugMessage(L"[INFO] 检测到旧进程已结束，清理句柄");
    CloseHandle(_hFaceRecognizerProcess);
    _hFaceRecognizerProcess = nullptr;
    _dwFaceRecognizerPID = 0;
  }

  // 2. 获取 FaceRecognizer.exe 的路径
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
      LogDebugMessage(L"[INFO] 从注册表读取 FaceRecognizer 路径: %s", szModuleDir);
    }
  }

  if (!useRegistryPath) {
    LogDebugMessage(L"[WARNING] 未能从注册表读取路径，使用DLL所在目录");
    wchar_t szFaceRecognizerPath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, szFaceRecognizerPath, MAX_PATH)) {
      LogDebugMessage(L"[ERROR] 获取模块路径失败");
      return HRESULT_FROM_WIN32(GetLastError());
    }
    wcscpy_s(szModuleDir, MAX_PATH, szFaceRecognizerPath);
    wchar_t *pLastSlash = wcsrchr(szModuleDir, L'\\');
    if (pLastSlash) {
      *(pLastSlash + 1) = L'\0';
    }
  }

  // 3. 拼接exe路径并检查
  wcscpy_s(szExePath, MAX_PATH, szModuleDir);
  wcscat_s(szExePath, MAX_PATH, L"FaceRecognizer.exe");

  DWORD attrib = GetFileAttributesW(szExePath);
  if (attrib == INVALID_FILE_ATTRIBUTES) {
    LogDebugMessage(L"[ERROR] FaceRecognizer.exe不存在: %s", szExePath);
    return E_FAIL;
  }

  // 4. 准备启动参数
  wchar_t szCmdLine[MAX_PATH * 2];
  swprintf_s(szCmdLine, MAX_PATH * 2, L"\"%s\" --mode %s", szExePath, pszMode);
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
  _hFaceRecognizerProcess = pi.hProcess;
  _dwFaceRecognizerPID = pi.dwProcessId;
  LogDebugMessage(L"[INFO] FaceRecognizer进程已启动，PID: %u, 模式: %s", pi.dwProcessId, pszMode);

  CloseHandle(pi.hThread);
  return S_OK;
}

HRESULT CSampleCredential::WaitForFaceRecognitionResult() {
  const int RECOGNITION_TIMEOUT_MS = 30000; // 30秒超时
  const int CHECK_INTERVAL_MS = 100;        // 每100ms检查一次
  int elapsed = 0;

  {
    std::lock_guard<std::mutex> lock(_faceMutex);
    _fFaceRecognitionRunning = true;
  }

  // 外部全局变量,由UDP接收器更新
  extern std::atomic<RecognitionStatus> face_recognition_status;

  LogDebugMessage(L"[INFO] 开始等待人脸识别结果，超时时间: %dms", RECOGNITION_TIMEOUT_MS);
  LogDebugMessage(L"[DEBUG] 初始状态码: %d", static_cast<int>(face_recognition_status.load()));

  while (elapsed < RECOGNITION_TIMEOUT_MS) {
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
    if (elapsed % 1000 == 0) {
      LogDebugMessage(L"[DEBUG] 等待中... 已耗时: %dms，当前状态码: %d",
                      elapsed, static_cast<int>(currentStatus));
    }

    Sleep(CHECK_INTERVAL_MS);
    elapsed += CHECK_INTERVAL_MS;
  }

  {
    std::lock_guard<std::mutex> lock(_faceMutex);
    _fFaceRecognitionRunning = false;
  }

  LogDebugMessage(L"[ERROR] 人脸识别超时或失败，耗时: %dms，最后状态码: %d，请检查FaceRecognizer是否正常运行",
                  elapsed, static_cast<int>(face_recognition_status.load()));
  return E_FAIL; // 超时或失败
}

HRESULT CSampleCredential::DecryptPasswordFromRegistry(PWSTR *ppwszPassword) {
  *ppwszPassword = nullptr;

  try {
    LogDebugMessage(L"[INFO] 开始从注册表读取加密数据...");
    
    // 从注册表读取加密的密码、密钥和IV
    std::string passwordHex = RegistryHelper::ReadStringFromRegistry(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\password");
    std::string keyHex = RegistryHelper::ReadStringFromRegistry(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\key");
    std::string ivHex = RegistryHelper::ReadStringFromRegistry(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Smile2Unlock_v2\\iv");

    // 检查是否成功读取
    if (passwordHex.empty() || keyHex.empty() || ivHex.empty()) {
      LogDebugMessage(L"[ERROR] 从注册表读取数据失败: password=%s, key=%s, iv=%s",
                     passwordHex.empty() ? L"空" : L"有", 
                     keyHex.empty() ? L"空" : L"有",
                     ivHex.empty() ? L"空" : L"有");
      return E_FAIL;
    }
    
    LogDebugMessage(L"[INFO] 注册表数据读取成功，长度: pwd=%zu, key=%zu, iv=%zu",
                   passwordHex.length(), keyHex.length(), ivHex.length());

    // 将十六进制密文转换为二进制
    std::vector<CryptoPP::byte> passwordCipherBytes =
        Crypto::hexToBytes(passwordHex);
    std::string passwordCipher(
        reinterpret_cast<const char *>(passwordCipherBytes.data()),
        passwordCipherBytes.size());

    LogDebugMessage(L"[INFO] 十六进制转换完成，密文长度: %zu", passwordCipher.length());

    // 创建Crypto对象并设置密钥和IV
    Crypto crypto;
    if (!crypto.setKeyHex(keyHex)) {
      LogDebugMessage(L"[ERROR] 密钥设置失败");
      return E_FAIL;
    }
    if (!crypto.setIvHex(ivHex)) {
      LogDebugMessage(L"[ERROR] IV设置失败");
      return E_FAIL;
    }
    
    LogDebugMessage(L"[INFO] 密钥和IV设置成功，开始解密...");

    // 解密
    std::string decryptedPassword = crypto.decrypt(passwordCipher);
    if (decryptedPassword.empty()) {
      LogDebugMessage(L"[ERROR] 解密失败或密码为空");
      return E_FAIL;
    }
    
    LogDebugMessage(L"[INFO] 解密成功，密码长度: %zu", decryptedPassword.length());

    // 转换为宽字符
    int bufferSize = MultiByteToWideChar(CP_UTF8, 0, decryptedPassword.c_str(),
                                         -1, nullptr, 0);
    if (bufferSize == 0) {
      LogDebugMessage(L"[ERROR] 计算宽字符缓冲区大小失败");
      return HRESULT_FROM_WIN32(GetLastError());
    }

    PWSTR pwszPassword = (PWSTR)CoTaskMemAlloc(bufferSize * sizeof(wchar_t));
    if (!pwszPassword) {
      LogDebugMessage(L"[ERROR] 内存分配失败");
      return E_OUTOFMEMORY;
    }

    if (!MultiByteToWideChar(CP_UTF8, 0, decryptedPassword.c_str(), -1,
                             pwszPassword, bufferSize)) {
      CoTaskMemFree(pwszPassword);
      LogDebugMessage(L"[ERROR] 多字节字符转宽字符失败");
      return HRESULT_FROM_WIN32(GetLastError());
    }

    *ppwszPassword = pwszPassword;
    LogDebugMessage(L"[INFO] 密码转换为宽字符成功");

    // 立即清零密码缓冲区
    SecureZeroMemory((PVOID)decryptedPassword.data(), decryptedPassword.size());
    LogDebugMessage(L"[INFO] 密码缓冲区已清零");

    return S_OK;
  } catch (const std::exception &e) {
    LogDebugMessage(L"[ERROR] 异常捕获: %hs", e.what());
    return E_FAIL;
  }
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
    if (_hFaceRecognizerProcess == nullptr) {
        return false;
    }

    DWORD dwExitCode;
    if (GetExitCodeProcess(_hFaceRecognizerProcess, &dwExitCode)) {
        return dwExitCode == STILL_ACTIVE;
    }

    return false;
}

HRESULT CSampleCredential::TerminateFaceRecognizer() {
    if (_hFaceRecognizerProcess == nullptr) {
        return S_OK;
    }

    // 检查进程是否仍在运行
    DWORD dwExitCode;
    if (GetExitCodeProcess(_hFaceRecognizerProcess, &dwExitCode)) {
        if (dwExitCode == STILL_ACTIVE) {
            LogDebugMessage(L"[INFO] FaceRecognizer进程仍在运行，等待其自然退出");

            // 等待进程自然退出（超时3秒）
            DWORD waitResult = WaitForSingleObject(_hFaceRecognizerProcess, 3000);
            if (waitResult == WAIT_TIMEOUT) {
                LogDebugMessage(L"[WARNING] 进程未在3秒内退出，尝试强制终止");
                if (!TerminateProcess(_hFaceRecognizerProcess, 1)) {
                    DWORD dwError = GetLastError();
                    // ACCESS_DENIED 是常见的，可能是权限不足或进程已退出
                    if (dwError == ERROR_ACCESS_DENIED) {
                        LogDebugMessage(L"[INFO] 无法终止进程（权限不足或已退出），忽略");
                    } else {
                        LogDebugMessage(L"[WARNING] 无法强制终止进程，错误代码: 0x%08X", dwError);
                    }
                } else {
                    LogDebugMessage(L"[INFO] FaceRecognizer进程已被强制终止");
                }
            } else if (waitResult == WAIT_OBJECT_0) {
                LogDebugMessage(L"[INFO] FaceRecognizer进程已自然退出");
            }
        } else {
            LogDebugMessage(L"[INFO] FaceRecognizer进程已退出，退出代码: %d", dwExitCode);
        }
    }

    return S_OK;
}

HRESULT CSampleCredential::StartFaceRecognitionAsync() {
    {
        std::lock_guard<std::mutex> lock(_faceMutex);
        if (_fFaceRecognitionRunning) {
            LogDebugMessage(L"[INFO] 人脸识别已在运行，忽略重复请求");
            return S_OK;
        }
    }

    // 启动识别线程
    try {
        // 关闭旧线程
        if (_faceRecognitionThread.joinable()) {
            _faceRecognitionThread.join();
        }

        // 重置识别状态
        extern std::atomic<RecognitionStatus> face_recognition_status;
        face_recognition_status = RecognitionStatus::IDLE;

        // 设置运行标志
        {
            std::lock_guard<std::mutex> lock(_faceMutex);
            _fFaceRecognitionRunning = true;
        }

        // 启动新线程
        _faceRecognitionThread = std::thread([this]() {
            LogDebugMessage(L"[INFO] 启动人脸识别线程");

            // 第一步：如果启用了预热模式，先启动预热
            if (_fWarmupModeEnabled) {
              std::this_thread::sleep_for(std::chrono::milliseconds(500));
              LogDebugMessage(L"[INFO] 启用预热模式，先进行轻量级人脸检测");
              HRESULT hr = LaunchFaceRecognizer(L"warmup");
              if (SUCCEEDED(hr)) {
                // 无限期等待预热完成（检测到人脸、用户切换、锁屏或系统休眠时退出）
                const int CHECK_INTERVAL_MS = 100;

                extern std::atomic<RecognitionStatus> face_recognition_status;
                while (true) {
                  // 检查识别是否被取消
                  {
                    std::lock_guard<std::mutex> lock(_faceMutex);
                    if (!_fFaceRecognitionRunning) {
                      LogDebugMessage(L"[INFO] 预热模式被取消");
                      break;
                    }
                  }

                  RecognitionStatus status = face_recognition_status.load();
                  if (status == RecognitionStatus::FACE_DETECTED) {
                    LogDebugMessage(
                        L"[INFO] 预热模式检测到人脸，切换到完整识别");
                    face_recognition_status = RecognitionStatus::IDLE;
                    break;
                  }
                  
                  std::this_thread::sleep_for(
                      std::chrono::milliseconds(CHECK_INTERVAL_MS));
                }

                // 终止预热进程
                TerminateFaceRecognizer();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
              }
            }

            // 第二步：启动完整识别
            LogDebugMessage(L"[INFO] 启动完整人脸识别");
            HRESULT hr = LaunchFaceRecognizer(L"recognize");
            if (SUCCEEDED(hr)) {
                HRESULT waitResult = WaitForFaceRecognitionResult();

                if (SUCCEEDED(waitResult)) {
                    LogDebugMessage(L"[INFO] 人脸识别成功，开始解密密码");

                    // 识别成功，解密密码
                    PWSTR pwszPassword = nullptr;
                    HRESULT decryptResult = DecryptPasswordFromRegistry(&pwszPassword);

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
                            _pCredProvCredentialEvents->BeginFieldUpdates();
                            _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, pwszPassword);
                            _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT, L"人脸识别成功，正在登录...");
                            _pCredProvCredentialEvents->EndFieldUpdates();

                            // 通知Provider凭证已准备好
                            if (_pProvider) {
                                LogDebugMessage(L"[INFO] 通知Provider凭证已准备好");
                                _pProvider->OnCredentialReady();
                            }
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
                LogDebugMessage(L"[ERROR] 启动FaceRecognizer失败: 0x%08X", hr);
            }

            // 清理
            {
                std::lock_guard<std::mutex> lock(_faceMutex);
                _fFaceRecognitionRunning = false;
            }
            LogDebugMessage(L"[INFO] 人脸识别线程结束");
        });

        return S_OK;
    } catch (const std::exception& e) {
        LogDebugMessage(L"[ERROR] 启动识别线程异常");
        return E_FAIL;
    }
}

void CSampleCredential::StopFaceRecognition() {
    LogDebugMessage(L"[INFO] 停止人脸识别");

    // 终止进程
    TerminateFaceRecognizer();

    // 标记为不运行
    {
        std::lock_guard<std::mutex> lock(_faceMutex);
        _fFaceRecognitionRunning = false;
    }

    // 等待线程结束（超时5秒）
    if (_faceRecognitionThread.joinable()) {
        auto thread_future = std::async(std::launch::async, [this]() {
            _faceRecognitionThread.join();
        });
        auto status = thread_future.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::timeout) {
            LogDebugMessage(L"[WARNING] 识别线程无响应，继续清理");
        }
    }

    LogDebugMessage(L"[INFO] 人脸识别已停止");
}
