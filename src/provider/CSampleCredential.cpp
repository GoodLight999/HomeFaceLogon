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
#include "CSampleProvider.h"
#include "guid.h"
#include "log.h"
#include "config.h"

// Tracks if we are currently signing in using face auth to prevent multiple host launches.
static bool g_fAutoLogonInProgress = false;
static ULONGLONG g_llLastAuthSuccessTime = 0;

// Tracks PIN attempts globally across instances to prevent brute force lock bypass by tile toggling
static int g_nPinAttempts = 0;
static ULONGLONG g_lockoutExpiration = 0;

CSampleCredential::CSampleCredential():
    _cRef(1),
    _pProvider(nullptr),
    _pCredProvCredentialEvents(nullptr),
    _pszUserSid(nullptr),
    _pszQualifiedUserName(nullptr),
    _fIsLocalUser(false),
    _fFaceAuthSuccess(false),
    _llCreationTime(0),
    _hHostProcess(INVALID_HANDLE_VALUE),
    _hHostJob(INVALID_HANDLE_VALUE),
    _hPipe(INVALID_HANDLE_VALUE),
    _hPipeEvent(nullptr),
    _hPipeReadThread(INVALID_HANDLE_VALUE),
    _sessionNonceHi(0),
    _sessionNonceLo(0),
    _fScanning(false)
{
    DllAddRef();

    ZeroMemory(_rgCredProvFieldDescriptors, sizeof(_rgCredProvFieldDescriptors));
    ZeroMemory(_rgFieldStatePairs, sizeof(_rgFieldStatePairs));
    ZeroMemory(_rgFieldStrings, sizeof(_rgFieldStrings));
}

CSampleCredential::~CSampleCredential()
{
    _StopHostProcess();

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
        hr = SHStrDupW(L"Home Face Logon", &_rgFieldStrings[SFI_LARGE_TEXT]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"", &_rgFieldStrings[SFI_PASSWORD]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"サインイン", &_rgFieldStrings[SFI_SUBMIT_BUTTON]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"顔照合を再試行", &_rgFieldStrings[SFI_LAUNCHWINDOW_LINK]);
    }
    if (SUCCEEDED(hr))
    {
        hr = SHStrDupW(L"カメラの準備中...", &_rgFieldStrings[SFI_LOGONSTATUS_TEXT]);
    }
    if (SUCCEEDED(hr))
    {
        hr = pcpUser->GetStringValue(PKEY_Identity_QualifiedUserName, &_pszQualifiedUserName);
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
    _StopHostProcess();
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
    *pbAutoLogon = FALSE;
    _StartHostProcess();
    return S_OK;
}

// Similarly to SetSelected, LogonUI calls this when your tile was selected
// and now no longer is. The most common thing to do here (which we do below)
// is to clear out the password field.
HRESULT CSampleCredential::SetDeselected()
{
    _StopHostProcess();

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
        if (*ppwszStored != nullptr && dwFieldID == SFI_PASSWORD)
        {
            SecureZeroMemory(*ppwszStored, wcslen(*ppwszStored) * sizeof(wchar_t));
        }
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
HRESULT CSampleCredential::GetCheckboxValue(DWORD /*dwFieldID*/, _Out_ BOOL *pbChecked, _Outptr_result_nullonfailure_ PWSTR *ppwszLabel)
{
    *pbChecked = FALSE;
    *ppwszLabel = nullptr;
    return E_NOTIMPL;
}

// Sets whether the specified checkbox is checked or not.
HRESULT CSampleCredential::SetCheckboxValue(DWORD /*dwFieldID*/, BOOL /*bChecked*/)
{
    return E_NOTIMPL;
}

// Returns the number of items to be included in the combobox (pcItems), as well as the
// currently selected item (pdwSelectedItem).
HRESULT CSampleCredential::GetComboBoxValueCount(DWORD /*dwFieldID*/, _Out_ DWORD *pcItems, _Out_ DWORD *pdwSelectedItem)
{
    *pcItems = 0;
    *pdwSelectedItem = 0;
    return E_NOTIMPL;
}

// Called iteratively to fill the combobox with the string (ppwszItem) at index dwItem.
HRESULT CSampleCredential::GetComboBoxValueAt(DWORD /*dwFieldID*/, DWORD /*dwItem*/, _Outptr_result_nullonfailure_ PWSTR *ppwszItem)
{
    *ppwszItem = nullptr;
    return E_NOTIMPL;
}

// Called when the user changes the selected item in the combobox.
HRESULT CSampleCredential::SetComboBoxSelectedValue(DWORD /*dwFieldID*/, DWORD /*dwSelectedItem*/)
{
    return E_NOTIMPL;
}

// Called when the user clicks a command link.
HRESULT CSampleCredential::CommandLinkClicked(DWORD dwFieldID)
{
    HRESULT hr = S_OK;

    if (dwFieldID < ARRAYSIZE(_rgCredProvFieldDescriptors) &&
        (CPFT_COMMAND_LINK == _rgCredProvFieldDescriptors[dwFieldID].cpft))
    {
        if (dwFieldID == SFI_LAUNCHWINDOW_LINK)
        {
            _StopHostProcess();
            _StartHostProcess();
        }
        else
        {
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
    LogInfo(L"GetSerialization called: _fIsLocalUser=%d, QualifiedUserName='%s'", _fIsLocalUser, _pszQualifiedUserName ? _pszQualifiedUserName : L"NULL");
    _StopHostProcess();

    if (_fFaceAuthSuccess)
    {
        g_fAutoLogonInProgress = true;
        g_llLastAuthSuccessTime = GetTickCount64();
    }

    HRESULT hr = E_UNEXPECTED;
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    ZeroMemory(pcpcs, sizeof(*pcpcs));

    // Check lockout first
    if (g_nPinAttempts >= 5)
    {
        ULONGLONG now = GetTickCount64();
        if (now < g_lockoutExpiration)
        {
            *ppwszOptionalStatusText = nullptr;
            SHStrDupW(L"PIN の入力に何度も失敗したため、一時的にロックされています。30秒後に再試行してください。", ppwszOptionalStatusText);
            return E_ACCESSDENIED;
        }
        else
        {
            g_nPinAttempts = 0;
            g_lockoutExpiration = 0;
        }
    }

    bool authValid = false;

    // Case 1: User submitted a PIN (password field is not empty)
    if (_rgFieldStrings[SFI_PASSWORD] && _rgFieldStrings[SFI_PASSWORD][0] != L'\0')
    {
        if (VerifyPin(_rgFieldStrings[SFI_PASSWORD]))
        {
            authValid = true;
            g_nPinAttempts = 0;
        }
        else
        {
            g_nPinAttempts++;
            if (g_nPinAttempts >= 5)
            {
                g_lockoutExpiration = GetTickCount64() + 30000; // 30 seconds
                *ppwszOptionalStatusText = nullptr;
                SHStrDupW(L"PIN が正しくありません。5回連続で失敗したため、30秒間ロックされます。", ppwszOptionalStatusText);
            }
            else
            {
                wchar_t szErr[128];
                StringCchPrintfW(szErr, ARRAYSIZE(szErr), L"PIN が正しくありません。残り %d 回入力できます。", 5 - g_nPinAttempts);
                *ppwszOptionalStatusText = nullptr;
                SHStrDupW(szErr, ppwszOptionalStatusText);
            }
            // Clear the password field in the UI
            CoTaskMemFree(_rgFieldStrings[SFI_PASSWORD]);
            SHStrDupW(L"", &_rgFieldStrings[SFI_PASSWORD]);
            if (_pCredProvCredentialEvents)
            {
                _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, _rgFieldStrings[SFI_PASSWORD]);
            }
            return E_ACCESSDENIED;
        }
    }
    // Case 2: Auto-logon triggered by successful face auth
    else if (_fFaceAuthSuccess)
    {
        authValid = true;
    }
    // Case 3: Neither PIN entered nor face auth succeeded (user just clicked submit button with empty PIN field)
    else
    {
        *ppwszOptionalStatusText = nullptr;
        SHStrDupW(L"PIN を入力するか、顔認証を行ってください。", ppwszOptionalStatusText);
        return E_ACCESSDENIED;
    }

    if (!authValid)
    {
        return E_ACCESSDENIED;
    }

    PWSTR pwzDecryptedPassword = nullptr;
    if (!LoadAndDecryptPassword(&pwzDecryptedPassword))
    {
        LogError(L"GetSerialization failed: Could not load or decrypt password from secret.bin");
        return E_FAIL;
    }

    if (_fIsLocalUser)
    {
        PWSTR pwzProtectedPassword;
        hr = ProtectIfNecessaryAndCopyPassword(pwzDecryptedPassword, _cpus, &pwzProtectedPassword);
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
                    hr = KerbInteractiveUnlockLogonPack(kiul, &pcpcs->rgbSerialization, &pcpcs->cbSerialization);
                    if (SUCCEEDED(hr))
                    {
                        ULONG ulAuthPackage;
                        hr = RetrieveNegotiateAuthPackage(&ulAuthPackage);
                        if (SUCCEEDED(hr))
                        {
                            pcpcs->ulAuthenticationPackage = ulAuthPackage;
                            pcpcs->clsidCredentialProvider = CLSID_CSample;
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

        if (!CredPackAuthenticationBuffer(dwAuthFlags, _pszQualifiedUserName, pwzDecryptedPassword, nullptr, &pcpcs->cbSerialization) &&
            (GetLastError() == ERROR_INSUFFICIENT_BUFFER))
        {
            pcpcs->rgbSerialization = static_cast<byte *>(CoTaskMemAlloc(pcpcs->cbSerialization));
            if (pcpcs->rgbSerialization != nullptr)
            {
                hr = S_OK;

                if (CredPackAuthenticationBuffer(dwAuthFlags, _pszQualifiedUserName, pwzDecryptedPassword, pcpcs->rgbSerialization, &pcpcs->cbSerialization))
                {
                    ULONG ulAuthPackage;
                    hr = RetrieveNegotiateAuthPackage(&ulAuthPackage);
                    if (SUCCEEDED(hr))
                    {
                        pcpcs->ulAuthenticationPackage = ulAuthPackage;
                        pcpcs->clsidCredentialProvider = CLSID_CSample;
                        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
                    }
                }
                else
                {
                    hr = HRESULT_FROM_WIN32(GetLastError());
                    LogError(L"CredPackAuthenticationBuffer failed: HRESULT=0x%08X", hr);
                    CoTaskMemFree(pcpcs->rgbSerialization);
                    pcpcs->rgbSerialization = nullptr;
                    pcpcs->cbSerialization = 0;
                }
            }
            else
            {
                hr = E_OUTOFMEMORY;
            }
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            LogError(L"CredPackAuthenticationBuffer size query failed: HRESULT=0x%08X", hr);
        }
    }

    if (pwzDecryptedPassword != nullptr)
    {
        SecureZeroMemory(pwzDecryptedPassword, wcslen(pwzDecryptedPassword) * sizeof(wchar_t));
        CoTaskMemFree(pwzDecryptedPassword);
    }

    _fFaceAuthSuccess = false;

    LogInfo(L"GetSerialization finished: hr=0x%08X, pcpgsr=%d, cbSerialization=%lu, ulAuthPackage=%lu", 
            hr, *pcpgsr, pcpcs->cbSerialization, pcpcs->ulAuthenticationPackage);
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
    { STATUS_LOGON_FAILURE, STATUS_SUCCESS, L"保存済みパスワードが無効です。標準のサインイン方法でログイン後、Face Logon設定を更新してください。", CPSI_ERROR, },
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
    _StopHostProcess();

    LogInfo(L"ReportResult: ntsStatus=0x%08X, ntsSubstatus=0x%08X", ntsStatus, ntsSubstatus);
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

    // Reset face auth success flag
    _fFaceAuthSuccess = false;

    // If we failed the logon, try to erase the password field.
    if (FAILED(HRESULT_FROM_NT(ntsStatus)))
    {
        g_fAutoLogonInProgress = false; // Reset lock on logon failure
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

#include <bcrypt.h>
#pragma comment(lib, "Bcrypt.lib")

HRESULT CSampleCredential::_StartHostProcess()
{
    // If auto-logon is currently in progress, skip launching host to prevent duplicate camera usage
    if (g_fAutoLogonInProgress)
    {
        if (GetTickCount64() - g_llLastAuthSuccessTime < 15000) // 15s window
        {
            LogInfo(L"Auto-logon is in progress. Skipping helper process startup.");
            return S_OK;
        }
        else
        {
            g_fAutoLogonInProgress = false;
        }
    }

    LogInfo(L"Starting FaceLogonHost helper process...");
    _llCreationTime = GetTickCount64();
    
    // Reset status text to clear any stale error from previous run
    _UpdateStatusText(L"カメラの準備中...");

    // Stop any existing process first
    _StopHostProcess();

    // 1. Generate Nonce
    NTSTATUS statusHi = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&_sessionNonceHi), sizeof(_sessionNonceHi), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    NTSTATUS statusLo = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&_sessionNonceLo), sizeof(_sessionNonceLo), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (statusHi != 0 || statusLo != 0) // STATUS_SUCCESS = 0
    {
        LogError(L"BCryptGenRandom failed to generate secure session nonce.");
        return E_FAIL;
    }

    wchar_t nonceHex[33];
    StringCchPrintfW(nonceHex, ARRAYSIZE(nonceHex), L"%016llx%016llx", _sessionNonceHi, _sessionNonceLo);

    // 2. Setup Named Pipe
    wchar_t pipeName[128];
    StringCchPrintfW(pipeName, ARRAYSIZE(pipeName), L"\\\\.\\pipe\\HomeFaceLogonPipe_%lu_%s", GetCurrentProcessId(), nonceHex);

    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR psd = nullptr;
    HRESULT hr = InitializeSecureSecurityAttributes(&sa, &psd);
    if (FAILED(hr))
    {
        LogError(L"InitializeSecureSecurityAttributes failed: 0x%08X", hr);
        return hr;
    }

    _hPipe = CreateNamedPipeW(
        pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        1024,
        1024,
        0,
        &sa
    );

    FreeSecureSecurityAttributes(&sa, psd);

    if (_hPipe == INVALID_HANDLE_VALUE)
    {
        LogError(L"CreateNamedPipeW failed: %lu", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    _hPipeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (_hPipeEvent == nullptr)
    {
        LogError(L"CreateEventW failed for _hPipeEvent: %lu", GetLastError());
        CloseHandle(_hPipe);
        _hPipe = INVALID_HANDLE_VALUE;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // 3. Create Job Object to tie host life to Winlogon/CP
    _hHostJob = CreateJobObjectW(nullptr, nullptr);
    if (_hHostJob != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(_hHostJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    // 4. Resolve host executable path
    // Under Winlogon, it resides in C:\Program Files\HomeFaceLogon\FaceLogonHost.exe
    // During dev, fallback to exe relative directory or current workspace path
    wchar_t hostPath[MAX_PATH] = L"C:\\Program Files\\HomeFaceLogon\\FaceLogonHost.exe";
    if (!PathFileExistsW(hostPath))
    {
        // Try relative to DLL path
        HMODULE hDll = GetModuleHandleW(L"FaceLogonProvider.dll");
        if (hDll && GetModuleFileNameW(hDll, hostPath, MAX_PATH))
        {
            PathRemoveFileSpecW(hostPath);
            PathAppendW(hostPath, L"FaceLogonHost.exe");
        }
    }
    if (!PathFileExistsW(hostPath))
    {
        // Debug fallback
        HMODULE hDll = GetModuleHandleW(L"FaceLogonProvider.dll");
        if (hDll && GetModuleFileNameW(hDll, hostPath, MAX_PATH))
        {
            PathRemoveFileSpecW(hostPath); // Debug or Release
            PathRemoveFileSpecW(hostPath); // x64
            PathRemoveFileSpecW(hostPath); // Workspace Root
            PathAppendW(hostPath, L"x64\\Debug\\FaceLogonHost.exe");
            if (!PathFileExistsW(hostPath))
            {
                // Try Release
                PathRemoveFileSpecW(hostPath);
                PathAppendW(hostPath, L"Release\\FaceLogonHost.exe");
            }
        }
    }

    LogInfo(L"Launching helper host path: %ls", hostPath);

    // 5. Build Command Line arguments
    AppConfig config = {};
    LoadAppConfig(&config);

    wchar_t cmdLine[512];
    StringCchPrintfW(cmdLine, ARRAYSIZE(cmdLine), L"\"%s\" --pipe \"%s\" --nonce %s --camera %d", hostPath, pipeName, nonceHex, config.cameraIndex);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(
        nullptr,
        cmdLine,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!ok)
    {
        LogError(L"CreateProcessW failed: %lu", GetLastError());
        CloseHandle(_hPipe);
        _hPipe = INVALID_HANDLE_VALUE;
        if (_hHostJob != INVALID_HANDLE_VALUE)
        {
            CloseHandle(_hHostJob);
            _hHostJob = INVALID_HANDLE_VALUE;
        }
        _UpdateStatusText(L"ヘルパープロセスの起動に失敗しました");
        return HRESULT_FROM_WIN32(GetLastError());
    }

    _hHostProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    if (_hHostJob != INVALID_HANDLE_VALUE)
    {
        AssignProcessToJobObject(_hHostJob, _hHostProcess);
    }

    _fScanning = true;
    _fFaceAuthSuccess = false;

    // 6. Start Named Pipe Read Thread
    _hPipeReadThread = CreateThread(nullptr, 0, _PipeReadThreadProc, this, 0, nullptr);
    
    _UpdateStatusText(L"カメラを準備しています...");
    return S_OK;
}

void CSampleCredential::_StopHostProcess()
{
    _fScanning = false;

    // Signal the thread to cancel
    if (_hPipeEvent != nullptr)
    {
        SetEvent(_hPipeEvent);
    }

    // 1. Terminate or Shutdown Host Process
    if (_hHostProcess != INVALID_HANDLE_VALUE)
    {
        // Try sending MSG_SHUTDOWN message to pipe before force-killing
        if (_hPipe != INVALID_HANDLE_VALUE)
        {
            struct MessageHeader {
                uint32_t magic;
                uint16_t version;
                uint16_t type;
                uint32_t payloadSize;
                uint64_t sessionNonceHi;
                uint64_t sessionNonceLo;
            } header = {};
            header.magic = 0x4F4C4648; // 'HFLO'
            header.version = 1;
            header.type = 8; // MSG_SHUTDOWN
            header.sessionNonceHi = _sessionNonceHi;
            header.sessionNonceLo = _sessionNonceLo;

            DWORD bytesWritten = 0;
            OVERLAPPED writeOverlapped = {};
            writeOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (writeOverlapped.hEvent != nullptr)
            {
                WriteFile(_hPipe, &header, sizeof(header), &bytesWritten, &writeOverlapped);
                // Wait briefly for write to complete
                WaitForSingleObject(writeOverlapped.hEvent, 50);
                CloseHandle(writeOverlapped.hEvent);
            }
        }

        // Force kill if necessary after brief delay, or just let Job Object handle it
        TerminateProcess(_hHostProcess, 0);
        CloseHandle(_hHostProcess);
        _hHostProcess = INVALID_HANDLE_VALUE;
    }

    // 2. Clean up Job Object
    if (_hHostJob != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_hHostJob);
        _hHostJob = INVALID_HANDLE_VALUE;
    }

    // 3. Cancel and Close Named Pipe
    if (_hPipe != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(_hPipe, nullptr);
        DisconnectNamedPipe(_hPipe);
        CloseHandle(_hPipe);
        _hPipe = INVALID_HANDLE_VALUE;
    }

    // 4. Wait for Read Thread
    if (_hPipeReadThread != INVALID_HANDLE_VALUE)
    {
        WaitForSingleObject(_hPipeReadThread, 200);
        CloseHandle(_hPipeReadThread);
        _hPipeReadThread = INVALID_HANDLE_VALUE;
    }

    // 5. Clean up pipe event
    if (_hPipeEvent != nullptr)
    {
        CloseHandle(_hPipeEvent);
        _hPipeEvent = nullptr;
    }

    _sessionNonceHi = 0;
    _sessionNonceLo = 0;
}

DWORD WINAPI CSampleCredential::_PipeReadThreadProc(LPVOID lpParam)
{
    CSampleCredential* pThis = reinterpret_cast<CSampleCredential*>(lpParam);
    if (!pThis) return 0;

    LogInfo(L"Pipe read thread started.");

    HANDLE hConnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hConnectEvent)
    {
        LogError(L"Failed to create hConnectEvent: %lu", GetLastError());
        return 0;
    }

    OVERLAPPED connectOverlapped = {};
    connectOverlapped.hEvent = hConnectEvent;

    // ConnectNamedPipe using overlapped I/O
    BOOL connected = FALSE;
    if (ConnectNamedPipe(pThis->_hPipe, &connectOverlapped))
    {
        connected = TRUE;
    }
    else
    {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED)
        {
            connected = TRUE;
        }
        else if (err == ERROR_IO_PENDING)
        {
            // Wait for connection or cancellation event
            HANDLE waitHandles[2] = { hConnectEvent, pThis->_hPipeEvent };
            DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
            if (waitRes == WAIT_OBJECT_0)
            {
                DWORD bytesTransferred = 0;
                if (GetOverlappedResult(pThis->_hPipe, &connectOverlapped, &bytesTransferred, FALSE))
                {
                    connected = TRUE;
                }
                else
                {
                    LogError(L"GetOverlappedResult for ConnectNamedPipe failed: %lu", GetLastError());
                }
            }
            else
            {
                LogInfo(L"ConnectNamedPipe wait canceled or pipe event signaled.");
                CancelIoEx(pThis->_hPipe, &connectOverlapped);
            }
        }
        else
        {
            LogError(L"ConnectNamedPipe failed: %lu", err);
        }
    }

    CloseHandle(hConnectEvent);

    if (!connected || !pThis->_fScanning)
    {
        LogInfo(L"Pipe connection not established or scanning stopped. Thread exiting.");
        return 0;
    }

    LogInfo(L"Pipe client connected.");

    struct MessageHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t type;
        uint32_t payloadSize;
        uint64_t sessionNonceHi;
        uint64_t sessionNonceLo;
    };

    HANDLE hReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hReadEvent)
    {
        LogError(L"Failed to create hReadEvent: %lu", GetLastError());
        return 0;
    }

    OVERLAPPED readOverlapped = {};
    readOverlapped.hEvent = hReadEvent;

    while (pThis->_fScanning)
    {
        MessageHeader header = {};
        DWORD bytesRead = 0;
        ResetEvent(hReadEvent);
        
        BOOL ok = ReadFile(pThis->_hPipe, &header, sizeof(header), &bytesRead, &readOverlapped);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                HANDLE waitHandles[2] = { hReadEvent, pThis->_hPipeEvent };
                DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (waitRes == WAIT_OBJECT_0)
                {
                    if (GetOverlappedResult(pThis->_hPipe, &readOverlapped, &bytesRead, FALSE))
                    {
                        ok = TRUE;
                    }
                    else
                    {
                        // Read failed
                        break;
                    }
                }
                else
                {
                    LogInfo(L"ReadFile wait canceled or pipe event signaled.");
                    CancelIoEx(pThis->_hPipe, &readOverlapped);
                    break;
                }
            }
            else
            {
                // Other error
                break;
            }
        }

        if (ok && bytesRead == sizeof(header))
        {
            if (header.magic == 0x4F4C4648 &&
                header.sessionNonceHi == pThis->_sessionNonceHi &&
                header.sessionNonceLo == pThis->_sessionNonceLo)
            {
                pThis->_HandlePipeMessage(header.type);
            }
            else
            {
                LogError(L"Received message with invalid magic or session nonce.");
            }
        }
        else
        {
            // Incomplete read or error
            break;
        }
    }

    CloseHandle(hReadEvent);
    LogInfo(L"Pipe read thread exiting.");
    return 0;
}

void CSampleCredential::_HandlePipeMessage(uint16_t msgType)
{
    switch (msgType)
    {
    case 3: // MSG_STATUS
        _UpdateStatusText(L"顔を探しています...");
        break;
    case 4: // MSG_MATCHED
        _UpdateStatusText(L"顔が一致しました。サインインしています...");
        _fFaceAuthSuccess = true;
        g_fAutoLogonInProgress = true;
        g_llLastAuthSuccessTime = GetTickCount64();
        if (_pProvider != nullptr)
        {
            // スリープ解除直後など、LogonUIが完全にアクティブ化する前に
            // TriggerAutoLogon を呼び出すと無視されてハングする現象を防ぐため、
            // ホストプロセス開始から最低 1200ms 経過するのを保証する。
            ULONGLONG elapsed = GetTickCount64() - _llCreationTime;
            const ULONGLONG TARGET_ACTIVE_DELAY = 1200;
            if (elapsed < TARGET_ACTIVE_DELAY)
            {
                ULONGLONG sleepTime = TARGET_ACTIVE_DELAY - elapsed;
                if (sleepTime > 1000) sleepTime = 1000;
                Sleep(static_cast<DWORD>(sleepTime));
            }
            _pProvider->TriggerAutoLogon();
        }
        break;
    case 5: // MSG_NO_MATCH
        _UpdateStatusText(L"登録された顔と一致しません");
        break;
    case 6: // MSG_CAMERA_ERROR
        _UpdateStatusText(L"カメラを開けませんでした。カメラが他のアプリに使用されていないか確認してください。");
        break;
    case 7: // MSG_MODEL_ERROR
        _UpdateStatusText(L"顔認識モデルの初期化に失敗しました。");
        break;
    default:
        break;
    }
}

void CSampleCredential::_UpdateStatusText(PCWSTR pwszStatus)
{
    LogInfo(L"Status Update: %ls", pwszStatus);

    if (_rgFieldStrings[SFI_LOGONSTATUS_TEXT])
    {
        CoTaskMemFree(_rgFieldStrings[SFI_LOGONSTATUS_TEXT]);
        _rgFieldStrings[SFI_LOGONSTATUS_TEXT] = nullptr;
    }

    SHStrDupW(pwszStatus, &_rgFieldStrings[SFI_LOGONSTATUS_TEXT]);

    if (_pCredProvCredentialEvents)
    {
        _pCredProvCredentialEvents->SetFieldString(this, SFI_LOGONSTATUS_TEXT, _rgFieldStrings[SFI_LOGONSTATUS_TEXT]);
    }
}
