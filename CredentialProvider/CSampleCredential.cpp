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
#include <thread>
#include <atomic>
#include <windows.h>
#include <string>
#include <vector>
#include "CSampleCredential.h"
#include "guid.h"

CSampleCredential::CSampleCredential():
    _cRef(1),
    _pCredProvCredentialEvents(nullptr),
    _pszUserSid(nullptr),
    _pszQualifiedUserName(nullptr),
    _fIsLocalUser(false),
    _fChecked(false),
    _fShowControls(false),
    _dwComboIndex(0),
    _faceRecogRunning(false),
    _faceRecogSuccess(false),
    _hFaceRecogDll(nullptr),
    _pFR_InitializeCamera(nullptr),
    _pFR_FaceRecognition(nullptr),
    _pFR_GetSimilarity(nullptr),
    _pFR_GetRecognitionStatus(nullptr),
    _pFR_Cleanup(nullptr)
{
    DllAddRef();

    ZeroMemory(_rgCredProvFieldDescriptors, sizeof(_rgCredProvFieldDescriptors));
    ZeroMemory(_rgFieldStatePairs, sizeof(_rgFieldStatePairs));
    ZeroMemory(_rgFieldStrings, sizeof(_rgFieldStrings));
}

CSampleCredential::~CSampleCredential()
{
    // 停止人脸识别线程
    StopFaceRecognition();
    
    // 清理人脸识别DLL
    UnloadFaceRecognitionDll();
    
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

    return hr;
}

// LogonUI calls this in order to give us a callback in case we need to notify it of anything.
HRESULT CSampleCredential::Advise(_In_ ICredentialProviderCredentialEvents *pcpce)
{
    if (_pCredProvCredentialEvents != nullptr)
    {
        _pCredProvCredentialEvents->Release();
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
    OutputDebugString(L"[FaceRecog] SetSelected called - credential selected\n");
    
    // 启动人脸识别
    StartFaceRecognition();
    
    // 设置自动登录标志
    *pbAutoLogon = FALSE; // 初始不自动登录，等待人脸识别成功
    
    wchar_t logMsg[100];
    swprintf_s(logMsg, L"[FaceRecog] AutoLogon set to: %s\n", *pbAutoLogon ? L"TRUE" : L"FALSE");
    OutputDebugString(logMsg);
    
    return S_OK;
}

// Similarly to SetSelected, LogonUI calls this when your tile was selected
// and now no longer is. The most common thing to do here (which we do below)
// is to clear out the password field.
HRESULT CSampleCredential::SetDeselected()
{
    OutputDebugString(L"[FaceRecog] SetDeselected called - credential deselected\n");
    
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

    OutputDebugString(L"[FaceRecog] SetDeselected completed\n");
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

    // 检查人脸识别是否成功
    if (_faceRecogSuccess)
    {
        // 人脸识别成功，准备自动登录
        // 这里需要准备一个空的密码，因为人脸识别已经验证了用户身份
        // 设置一个默认密码（实际使用时应该根据人脸识别结果设置正确的凭证）
        if (_rgFieldStrings[SFI_PASSWORD] == nullptr || wcslen(_rgFieldStrings[SFI_PASSWORD]) == 0)
        {
            // 如果密码为空，设置一个默认密码
            CoTaskMemFree(_rgFieldStrings[SFI_PASSWORD]);
            SHStrDupW(L"", &_rgFieldStrings[SFI_PASSWORD]);
        }
        
        // 继续正常的登录流程
    }

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
    { STATUS_LOGON_FAILURE, STATUS_SUCCESS, L"Incorrect password or username.", CPSI_ERROR, },
    { STATUS_ACCOUNT_RESTRICTION, STATUS_ACCOUNT_DISABLED, L"The account is disabled.", CPSI_WARNING },
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

// 人脸识别相关方法实现

// 加载人脸识别DLL
bool CSampleCredential::LoadFaceRecognitionDll()
{
    if (_hFaceRecogDll != nullptr)
    {
        OutputDebugString(L"[FaceRecog] DLL already loaded\n");
        return true; // 已经加载
    }
    
    // 尝试多个可能的DLL路径
    const wchar_t* dllPaths[] = {
        L"FaceRecognizelib.dll",
        L"build\\windows\\x64\\release\\FaceRecognizelib.dll",
        L"..\\build\\windows\\x64\\release\\FaceRecognizelib.dll"
    };
    
    for (int i = 0; i < 3; i++)
    {
        wchar_t debugMsg[200];
        swprintf_s(debugMsg, L"[FaceRecog] Trying to load DLL from: %s\n", dllPaths[i]);
        OutputDebugString(debugMsg);
        
        _hFaceRecogDll = LoadLibrary(dllPaths[i]);
        if (_hFaceRecogDll != nullptr)
        {
            swprintf_s(debugMsg, L"[FaceRecog] Successfully loaded DLL from: %s\n", dllPaths[i]);
            OutputDebugString(debugMsg);
            break;
        }
        else
        {
            DWORD error = GetLastError();
            swprintf_s(debugMsg, L"[FaceRecog] Failed to load from %s, error: %d\n", dllPaths[i], error);
            OutputDebugString(debugMsg);
        }
    }
    
    if (_hFaceRecogDll == nullptr)
    {
        OutputDebugString(L"[FaceRecog] Failed to load FaceRecognizelib.dll from all paths\n");
        return false;
    }
    OutputDebugString(L"[FaceRecog] FaceRecognizelib.dll loaded successfully\n");
    
    // 获取函数指针
    OutputDebugString(L"[FaceRecog] Getting function pointers...\n");
    _pFR_InitializeCamera = (FR_InitializeCameraFunc)GetProcAddress(_hFaceRecogDll, "FR_InitializeCamera");
    _pFR_FaceRecognition = (FR_FaceRecognitionFunc)GetProcAddress(_hFaceRecogDll, "FR_FaceRecognition");
    _pFR_GetSimilarity = (FR_GetSimilarityFunc)GetProcAddress(_hFaceRecogDll, "FR_GetSimilarity");
    _pFR_GetRecognitionStatus = (FR_GetRecognitionStatusFunc)GetProcAddress(_hFaceRecogDll, "FR_GetRecognitionStatus");
    _pFR_Cleanup = (FR_CleanupFunc)GetProcAddress(_hFaceRecogDll, "FR_Cleanup");
    
    // 检查所有函数是否成功加载
    if (_pFR_InitializeCamera == nullptr)
        OutputDebugString(L"[FaceRecog] Failed to get FR_InitializeCamera function\n");
    if (_pFR_FaceRecognition == nullptr)
        OutputDebugString(L"[FaceRecog] Failed to get FR_FaceRecognition function\n");
    if (_pFR_GetSimilarity == nullptr)
        OutputDebugString(L"[FaceRecog] Failed to get FR_GetSimilarity function\n");
    if (_pFR_GetRecognitionStatus == nullptr)
        OutputDebugString(L"[FaceRecog] Failed to get FR_GetRecognitionStatus function\n");
    if (_pFR_Cleanup == nullptr)
        OutputDebugString(L"[FaceRecog] Failed to get FR_Cleanup function\n");
    
    if (_pFR_InitializeCamera == nullptr || _pFR_FaceRecognition == nullptr || 
        _pFR_GetSimilarity == nullptr || _pFR_GetRecognitionStatus == nullptr || 
        _pFR_Cleanup == nullptr)
    {
        OutputDebugString(L"[FaceRecog] Failed to load all required functions\n");
        UnloadFaceRecognitionDll();
        return false;
    }
    
    OutputDebugString(L"[FaceRecog] All functions loaded successfully\n");
    return true;
}

// 卸载人脸识别DLL
void CSampleCredential::UnloadFaceRecognitionDll()
{
    if (_hFaceRecogDll != nullptr)
    {
        FreeLibrary(_hFaceRecogDll);
        _hFaceRecogDll = nullptr;
        _pFR_InitializeCamera = nullptr;
        _pFR_FaceRecognition = nullptr;
        _pFR_GetSimilarity = nullptr;
        _pFR_GetRecognitionStatus = nullptr;
        _pFR_Cleanup = nullptr;
    }
}

// 执行人脸识别
void CSampleCredential::PerformFaceRecognition()
{
    OutputDebugString(L"[FaceRecog] Starting face recognition thread...\n");
    
    // 加载人脸识别DLL
    if (!LoadFaceRecognitionDll())
    {
        OutputDebugString(L"[FaceRecog] Failed to load face recognition DLL\n");
        _faceRecogSuccess = false;
        return;
    }
    
    OutputDebugString(L"[FaceRecog] Initializing camera...\n");
    // 初始化摄像头
    if (!_pFR_InitializeCamera())
    {
        OutputDebugString(L"[FaceRecog] Failed to initialize camera - function returned false\n");
        _faceRecogSuccess = false;
        return;
    }
    OutputDebugString(L"[FaceRecog] Camera initialized successfully\n");
    
    // 检查数据文件是否存在
    std::vector<std::string> dataFilePaths;
    dataFilePaths.push_back("face_data.dat");
    dataFilePaths.push_back("face_data.bin");
    dataFilePaths.push_back("build\\windows\\x64\\release\\face_data.dat");
    dataFilePaths.push_back("build\\windows\\x64\\release\\face_data.bin");
    dataFilePaths.push_back("..\\build\\windows\\x64\\release\\face_data.dat");
    dataFilePaths.push_back("..\\build\\windows\\x64\\release\\face_data.bin");
    
    // 添加system32目录路径
    char system32Path[MAX_PATH];
    if (GetSystemDirectoryA(system32Path, MAX_PATH) > 0) {
        std::string system32FaceData = std::string(system32Path) + "\\face_data.bin";
        dataFilePaths.push_back(system32FaceData);
        OutputDebugStringA(("[FaceRecog] Checking system32 path: " + system32FaceData).c_str());
    }
    
    std::string dataFile;
    bool dataFileFound = false;
    
    for (const auto& path : dataFilePaths) {
        FILE* fileCheck = fopen(path.c_str(), "rb");
        if (fileCheck != nullptr) {
            dataFile = path;
            dataFileFound = true;
            OutputDebugStringA(("[FaceRecog] Data file found: " + dataFile).c_str());
            fclose(fileCheck);
            break;
        }
    }
    
    if (!dataFileFound) {
        OutputDebugStringA("[FaceRecog] Data file not found in any path");
        OutputDebugStringA("[FaceRecog] Failed to start face recognition - no data file");
        return;
    }
    
    OutputDebugString(L"[FaceRecog] Starting face recognition with data file...\n");
    // 开始人脸识别
    if (!_pFR_FaceRecognition(dataFile.c_str()))
    {
        wchar_t errorMsg[200];
        swprintf_s(errorMsg, L"[FaceRecog] Failed to start face recognition with data file: %hs\n", dataFile.c_str());
        OutputDebugString(errorMsg);
        _faceRecogSuccess = false;
        return;
    }
    OutputDebugString(L"[FaceRecog] Face recognition started successfully\n");
    
    OutputDebugString(L"[FaceRecog] Entering face recognition loop...\n");
    // 人脸识别循环
    int frameCount = 0;
    while (_faceRecogRunning)
    {
        frameCount++;
        
        // 检查识别状态
        int status = _pFR_GetRecognitionStatus();
        
        if (status == 2) // 识别成功
        {
            //TODO: 处理识别成功逻辑
        }
        else if (status == 3 || status == 4) // 识别失败或活体检测失败
        {
            wchar_t statusMsg[100];
            swprintf_s(statusMsg, L"[FaceRecog] Frame %d: Recognition failed, status: %d\n", frameCount, status);
            OutputDebugString(statusMsg);
            _faceRecogSuccess = false;
            break;
        }
        else if (status == 1) // 检测中
        {
            if (frameCount % 50 == 0) // 每50帧输出一次状态
            {
                wchar_t statusMsg[100];
                swprintf_s(statusMsg, L"[FaceRecog] Frame %d: Detecting face...\n", frameCount);
                OutputDebugString(statusMsg);
            }
        }
        
        // 短暂休眠，避免过度占用CPU
        Sleep(100);
    }
    
    if (!_faceRecogSuccess)
    {
        OutputDebugString(L"[FaceRecog] Face recognition failed or stopped\n");
    }
    
    OutputDebugString(L"[FaceRecog] Cleaning up resources...\n");
    // 清理资源
    _pFR_Cleanup();
    OutputDebugString(L"[FaceRecog] Face recognition thread finished\n");
}

// 启动人脸识别
void CSampleCredential::StartFaceRecognition()
{
    OutputDebugString(L"[FaceRecog] StartFaceRecognition called\n");
    
    if (_faceRecogRunning)
    {
        OutputDebugString(L"[FaceRecog] Face recognition is already running\n");
        return; // 已经在运行
    }
    
    _faceRecogRunning = true;
    _faceRecogSuccess = false;
    
    OutputDebugString(L"[FaceRecog] Creating face recognition thread...\n");
    // 启动人脸识别线程
    _faceRecogThread = std::thread(&CSampleCredential::PerformFaceRecognition, this);
    OutputDebugString(L"[FaceRecog] Face recognition thread created successfully\n");
}

// 停止人脸识别
void CSampleCredential::StopFaceRecognition()
{
    OutputDebugString(L"[FaceRecog] StopFaceRecognition called\n");
    
    if (!_faceRecogRunning)
    {
        OutputDebugString(L"[FaceRecog] Face recognition is not running\n");
        return; // 没有在运行
    }
    
    OutputDebugString(L"[FaceRecog] Stopping face recognition...\n");
    _faceRecogRunning = false;
    
    // 等待线程结束
    if (_faceRecogThread.joinable())
    {
        OutputDebugString(L"[FaceRecog] Waiting for thread to finish...\n");
        _faceRecogThread.join();
        OutputDebugString(L"[FaceRecog] Thread stopped successfully\n");
    }
    else
    {
        OutputDebugString(L"[FaceRecog] Thread is not joinable\n");
    }
    
    OutputDebugString(L"[FaceRecog] Cleaning up DLL resources...\n");
    // 清理DLL资源
    UnloadFaceRecognitionDll();
    OutputDebugString(L"[FaceRecog] StopFaceRecognition completed\n");
}