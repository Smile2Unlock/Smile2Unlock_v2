//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//

#ifndef WIN32_NO_STATUS
#include <ntstatus.h>
#define WIN32_NO_STATUS
#endif
#include <unknwn.h>
#include "CSampleCredential.h"
#include "guid.h"

// 新增包含
#include "Crypto.h"
#include "ipc_receiver.h"
#include "registryhelper.h"
#include <chrono>
#include <cwchar>
#include <mutex>
#include <thread>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>

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
  
  // 输出到文件
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
    _fFaceRecognitionRunning(false)
{
    DllAddRef();

    ZeroMemory(_rgCredProvFieldDescriptors, sizeof(_rgCredProvFieldDescriptors));
    ZeroMemory(_rgFieldStatePairs, sizeof(_rgFieldStatePairs));
    ZeroMemory(_rgFieldStrings, sizeof(_rgFieldStrings));
}

CSampleCredential::~CSampleCredential()
{
    // 停止人脸识别
    TerminateFaceRecognizer();

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
                                      _In_ ICredentialProviderUser *pcpUser)
{
    HRESULT hr = S_OK;
    _cpus = cpus;

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

    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"使用面部识别登录", &_rgFieldStrings[SFI_FACE_RECOGNITION_LINK]);
    }

    return hr;
}

// LogonUI calls this in order to give us a callback in case we need to notify it of anything.
HRESULT CSampleCredential::Advise(_In_ ICredentialProviderCredentialEvents *pcpce)
{
    LogDebugMessage(L"[INFO] CSampleCredential::Advise - 初始化IPC接收器");
    
    if (_pCredProvCredentialEvents != nullptr)
    {
        _pCredProvCredentialEvents->Release();
    }

    // 初始化IPC接收器
    if (!_pIpcReceiver)
    {
        _pIpcReceiver = std::make_unique<ipc_receiver>();
        LogDebugMessage(L"[INFO] IPC接收器创建成功");
    }

    return pcpce->QueryInterface(IID_PPV_ARGS(&_pCredProvCredentialEvents));
}

// LogonUI calls this to tell us to release the callback.
HRESULT CSampleCredential::UnAdvise()
{
    // 停止人脸识别
    TerminateFaceRecognizer();

    if (_pCredProvCredentialEvents)
    {
        _pCredProvCredentialEvents->Release();
    }
    _pCredProvCredentialEvents = nullptr;

    // 清理IPC接收器
    _pIpcReceiver.reset();

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
    return S_OK;
}

// Similarly to SetSelected, LogonUI calls this when your tile was selected
// and now no longer is. The most common thing to do here (which we do below)
// is to clear out the password field.
HRESULT CSampleCredential::SetDeselected()
{
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

HRESULT CSampleCredential::LaunchFaceRecognizer() {
  // 构建FaceRecognizer.exe的路径
  wchar_t szFaceRecognizerPath[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, szFaceRecognizerPath, MAX_PATH)) {
    LogDebugMessage(L"[ERROR] 获取模块路径失败");
    return HRESULT_FROM_WIN32(GetLastError());
  }

  LogDebugMessage(L"[INFO] DLL模块路径: %s", szFaceRecognizerPath);

  // 获取当前DLL所在的目录
  wchar_t *pLastSlash = wcsrchr(szFaceRecognizerPath, L'\\');
  if (pLastSlash) {
    *(pLastSlash + 1) = L'\0';
  }

  // 拼接FaceRecognizer.exe的路径
  wcscat_s(szFaceRecognizerPath, MAX_PATH, L"FaceRecognizer.exe");

  LogDebugMessage(L"[INFO] 目标exe路径: %s", szFaceRecognizerPath);

  // 检查文件是否存在
  DWORD attrib = GetFileAttributesW(szFaceRecognizerPath);
  if (attrib == INVALID_FILE_ATTRIBUTES) {
    LogDebugMessage(L"[ERROR] FaceRecognizer.exe不存在: %s", szFaceRecognizerPath);
    return E_FAIL;
  }

  // 准备启动参数：--mode recognize
  wchar_t szCmdLine[MAX_PATH];
  wcscpy_s(szCmdLine, MAX_PATH, szFaceRecognizerPath);
  wcscat_s(szCmdLine, MAX_PATH, L" --mode recognize");

  LogDebugMessage(L"[INFO] 启动命令行: %s", szCmdLine);

  // 创建进程
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  if (!CreateProcessW(nullptr,          // lpApplicationName
                      szCmdLine,        // lpCommandLine
                      nullptr,          // lpProcessAttributes
                      nullptr,          // lpThreadAttributes
                      FALSE,            // bInheritHandles
                      CREATE_NO_WINDOW, // dwCreationFlags
                      nullptr,          // lpEnvironment
                      nullptr,          // lpCurrentDirectory
                      &si,              // lpStartupInfo
                      &pi))             // lpProcessInformation
  {
    DWORD dwError = GetLastError();
    LogDebugMessage(L"[ERROR] CreateProcessW失败，错误代码: 0x%08X", dwError);
    return HRESULT_FROM_WIN32(dwError);
  }

  // 保存进程句柄
  _hFaceRecognizerProcess = pi.hProcess;
  LogDebugMessage(L"[INFO] FaceRecognizer进程已启动，PID: %u", pi.dwProcessId);
  
  CloseHandle(pi.hThread); // 不需要线程句柄

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

  // 外部全局变量，由IPC接收器更新
  extern int face_reconizer_status;

  LogDebugMessage(L"[INFO] 开始等待人脸识别结果，超时时间: %dms", RECOGNITION_TIMEOUT_MS);

  while (elapsed < RECOGNITION_TIMEOUT_MS) {
    {
      std::lock_guard<std::mutex> lock(_faceMutex);
      if (!_fFaceRecognitionRunning) {
        LogDebugMessage(L"[INFO] 识别被取消");
        break;
      }
    }

    // 检查识别结果
    if (face_reconizer_status == 1) {
      LogDebugMessage(L"[INFO] 识别成功！耗时: %dms", elapsed);
      return S_OK; // 识别成功
    }

    Sleep(CHECK_INTERVAL_MS);
    elapsed += CHECK_INTERVAL_MS;
  }

  {
    std::lock_guard<std::mutex> lock(_faceMutex);
    _fFaceRecognitionRunning = false;
  }

  LogDebugMessage(L"[ERROR] 人脸识别超时或失败，耗时: %dms，最后状态码: %d", elapsed, face_reconizer_status);
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

void CSampleCredential::TerminateFaceRecognizer() {
  {
    std::lock_guard<std::mutex> lock(_faceMutex);
    _fFaceRecognitionRunning = false;
  }

  // 等待识别线程退出
  if (_faceRecognitionThread.joinable()) {
    _faceRecognitionThread.join();
  }

  // 结束FaceRecognizer进程
  if (_hFaceRecognizerProcess) {
    // 等待进程退出，超时5秒
    if (WaitForSingleObject(_hFaceRecognizerProcess, 5000) == WAIT_TIMEOUT) {
      // 如果超时，强制结束进程
      TerminateProcess(_hFaceRecognizerProcess, 1);
    }

    CloseHandle(_hFaceRecognizerProcess);
    _hFaceRecognizerProcess = nullptr;
  }
}

// Called when the user clicks a command link.
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
            if (_pCredProvCredentialEvents)
            {
                _pCredProvCredentialEvents->OnCreatingWindow(&hwndOwner);
            }

            // 重置识别状态
            extern int face_reconizer_status;
            face_reconizer_status = 0;

            // 1. 启动FaceRecognizer进程
            hr = LaunchFaceRecognizer();
            if (FAILED(hr))
            {
                LogDebugMessage(L"[ERROR] 启动人脸识别模块失败，错误代码: 0x%08X", hr);
                ::MessageBox(hwndOwner, L"启动人脸识别模块失败", L"错误", MB_ICONERROR);
                return hr;
            }
            
            LogDebugMessage(L"[INFO] 人脸识别进程已启动，等待识别结果...");

            // 显示等待提示
            if (_pCredProvCredentialEvents)
            {
                _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT,
                                                           L"正在进行人脸识别...");
            }

            // 2. 在后台线程中等待识别结果
            _faceRecognitionThread = std::thread([this, hwndOwner]() {
              LogDebugMessage(L"[INFO] 后台识别线程已启动");
              HRESULT waitResult = WaitForFaceRecognitionResult();

              if (SUCCEEDED(waitResult)) {
                LogDebugMessage(L"[INFO] 人脸识别成功，正在解密密码...");
                // 识别成功，从注册表解密密码
                PWSTR pwszPassword = nullptr;
                HRESULT decryptResult = DecryptPasswordFromRegistry(&pwszPassword);

                if (SUCCEEDED(decryptResult) && pwszPassword) {
                  LogDebugMessage(L"[INFO] 密码解密成功，正在设置密码字段...");
                  // 3. 设置密码到密码字段
                  {
                    std::lock_guard<std::mutex> lock(_faceMutex);
                    if (_rgFieldStrings[SFI_PASSWORD]) {
                      CoTaskMemFree(_rgFieldStrings[SFI_PASSWORD]);
                    }
                    _rgFieldStrings[SFI_PASSWORD] = pwszPassword;
                  }

                  if (_pCredProvCredentialEvents) {
                    _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD,
                                                               pwszPassword);
                    _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT,
                                                               L"人脸识别成功");
                    LogDebugMessage(L"[INFO] 密码已设置，等待LogonUI调用GetSerialization...");
                    // 注意：不需要显式调用OnUserAuthentication
                    // LogonUI会监听字段变化并自动调用GetSerialization
                  }
                } else {
                  LogDebugMessage(L"[ERROR] 密码解密失败，错误代码: 0x%08X", decryptResult);
                  ::MessageBox(hwndOwner, L"密码解密失败", L"错误", MB_ICONERROR);
                }
              } else {
                LogDebugMessage(L"[ERROR] 人脸识别失败或超时，原因：%s", 
                               waitResult == E_FAIL ? L"超时或识别失败" : L"异常错误");
                ::MessageBox(hwndOwner, L"人脸识别失败或超时，请重试", L"识别失败",
                             MB_ICONERROR);

                // 清空密码字段
                if (_pCredProvCredentialEvents) {
                  _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, L"");
                  _pCredProvCredentialEvents->SetFieldString(this, SFI_LARGE_TEXT,
                                                             L"人脸识别失败");
                }
              }

              // 终止FaceRecognizer进程
              LogDebugMessage(L"[INFO] 正在终止FaceRecognizer进程...");
              TerminateFaceRecognizer();
              LogDebugMessage(L"[INFO] 后台识别线程已结束");
            });

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

    // For local user, the domain and user name can be split from _pszQualifiedUserName (domain\username).
    // CredPackAuthenticationBuffer() cannot be used because it won't work with unlock scenario.
    if (_fIsLocalUser)
    {
        PWSTR pwzProtectedPassword;
        hr = ProtectIfNecessaryAndCopyPassword(_rgFieldStrings[SFI_PASSWORD], _cpus, &pwzProtectedPassword);
        if (SUCCEEDED(hr))
        {
            PWSTR pszDomain;
            PWSTR pszUsername;
            hr = SplitDomainAndUsername(_pszQualifiedUserName, &pszDomain, &pszUsername);
            if (SUCCEEDED(hr))
            {
                KERB_INTERACTIVE_UNLOCK_LOGON kiul;
                hr = KerbInteractiveUnlockLogonInit(pszDomain, pszUsername, pwzProtectedPassword, _cpus, &kiul);
                if (SUCCEEDED(hr))
                {
                    // We use KERB_INTERACTIVE_UNLOCK_LOGON in both unlock and logon scenarios.  It contains a
                    // KERB_INTERACTIVE_LOGON to hold the creds plus a LUID that is filled in for us by Winlogon
                    // as necessary.
                    hr = KerbInteractiveUnlockLogonPack(kiul, &pcpcs->rgbSerialization, &pcpcs->cbSerialization);
                    if (SUCCEEDED(hr))
                    {
                        ULONG ulAuthPackage;
                        hr = RetrieveNegotiateAuthPackage(&ulAuthPackage);
                        if (SUCCEEDED(hr))
                        {
                            pcpcs->ulAuthenticationPackage = ulAuthPackage;
                            pcpcs->clsidCredentialProvider = CLSID_CSample;
                            // At this point the credential has created the serialized credential used for logon
                            // By setting this to CPGSR_RETURN_CREDENTIAL_FINISHED we are letting logonUI know
                            // that we have all the information we need and it should attempt to submit the
                            // serialized credential.
                            *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
                        }
                    }
                }
                CoTaskMemFree(pszDomain);
                CoTaskMemFree(pszUsername);
            }
            CoTaskMemFree(pwzProtectedPassword);
        }
    }
    else
    {
        DWORD dwAuthFlags = CRED_PACK_PROTECTED_CREDENTIALS | CRED_PACK_ID_PROVIDER_CREDENTIALS;

        // First get the size of the authentication buffer to allocate
        if (!CredPackAuthenticationBuffer(dwAuthFlags, _pszQualifiedUserName, const_cast<PWSTR>(_rgFieldStrings[SFI_PASSWORD]), nullptr, &pcpcs->cbSerialization) &&
            (GetLastError() == ERROR_INSUFFICIENT_BUFFER))
        {
            pcpcs->rgbSerialization = static_cast<byte *>(CoTaskMemAlloc(pcpcs->cbSerialization));
            if (pcpcs->rgbSerialization != nullptr)
            {
                hr = S_OK;

                // Retrieve the authentication buffer
                if (CredPackAuthenticationBuffer(dwAuthFlags, _pszQualifiedUserName, const_cast<PWSTR>(_rgFieldStrings[SFI_PASSWORD]), pcpcs->rgbSerialization, &pcpcs->cbSerialization))
                {
                    ULONG ulAuthPackage;
                    hr = RetrieveNegotiateAuthPackage(&ulAuthPackage);
                    if (SUCCEEDED(hr))
                    {
                        pcpcs->ulAuthenticationPackage = ulAuthPackage;
                        pcpcs->clsidCredentialProvider = CLSID_CSample;

                        // At this point the credential has created the serialized credential used for logon
                        // By setting this to CPGSR_RETURN_CREDENTIAL_FINISHED we are letting logonUI know
                        // that we have all the information we need and it should attempt to submit the
                        // serialized credential.
                        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
                    }
                }
                else
                {
                    hr = HRESULT_FROM_WIN32(GetLastError());
                    if (SUCCEEDED(hr))
                    {
                        hr = E_FAIL;
                    }
                }

                if (FAILED(hr))
                {
                    CoTaskMemFree(pcpcs->rgbSerialization);
                }
            }
            else
            {
                hr = E_OUTOFMEMORY;
            }
        }
    }
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
    { STATUS_LOGON_FAILURE, STATUS_SUCCESS, const_cast<PWSTR>(L"Incorrect password or username."), CPSI_ERROR, },
    { STATUS_ACCOUNT_RESTRICTION, STATUS_ACCOUNT_DISABLED, const_cast<PWSTR>(L"The account is disabled."), CPSI_WARNING },
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
