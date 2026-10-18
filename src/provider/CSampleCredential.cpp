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

// Pipe workers only post messages. All credential/COM state belongs to LogonUI's thread.
static constexpr UINT WM_HOST_MESSAGE = WM_APP + 1;

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
    _fSubmitting(false),
    _faceMatchedAt(0),
    _llCreationTime(0),
    _hHostProcess(INVALID_HANDLE_VALUE),
    _hHostJob(INVALID_HANDLE_VALUE),
    _hPipe(INVALID_HANDLE_VALUE),
    _hPipeEvent(nullptr),
    _hPipeReadThread(INVALID_HANDLE_VALUE),
    _sessionNonceHi(0),
    _sessionNonceLo(0),
    _fScanning(false),
    _hMessageWindow(nullptr),
    _scanGeneration(0)
{
    DllAddRef();

    ZeroMemory(_rgCredProvFieldDescriptors, sizeof(_rgCredProvFieldDescriptors));
    ZeroMemory(_rgFieldStatePairs, sizeof(_rgFieldStatePairs));
    ZeroMemory(_rgFieldStrings, sizeof(_rgFieldStrings));
}

CSampleCredential::~CSampleCredential()
{
    _StopHostProcess();
    if (_hMessageWindow) DestroyWindow(_hMessageWindow);
    UnregisterClassW(L"HomeFaceLogonCredentialEvents", HINST_THISDLL);
    if (_pCredProvCredentialEvents) _pCredProvCredentialEvents->Release();

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
        hr = SHStrDupW(L"顔認証（HomeFaceLogon）", &_rgFieldStrings[SFI_PROVIDER_LABEL]);
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
    LogInfo(L"Credential Advise: face=%d, submitting=%d, scanning=%d", _fFaceAuthSuccess, _fSubmitting, _fScanning.load() ? 1 : 0);
    // Do not stop a running host here. LogonUI may call SetSelected before
    // (or re-issue) Advise; killing the session here caused the camera to
    // start, stop, and restart. The message window survives Advise, so an
    // in-flight scan keeps delivering results. UnAdvise still stops it.
    if (_pCredProvCredentialEvents != nullptr)
    {
        _pCredProvCredentialEvents->Release();
    }
    _pCredProvCredentialEvents = nullptr;
    HRESULT hr = _CreateMessageWindow();
    if (SUCCEEDED(hr)) hr = pcpce->QueryInterface(IID_PPV_ARGS(&_pCredProvCredentialEvents));
    return hr;
}

// LogonUI calls this to tell us to release the callback.
HRESULT CSampleCredential::UnAdvise()
{
    LogInfo(L"Credential UnAdvise: face=%d, submitting=%d", _fFaceAuthSuccess, _fSubmitting);
    _StopHostProcess();
    // CredentialsChanged re-enumerates the tile and replaces its event callback
    // before GetCredentialCount/GetSerialization. Releasing that callback must
    // not consume the face proof or cancel an already packaged submission.
    if (!_fFaceAuthSuccess && !_fSubmitting) _ResetAuthentication();
    if (_pCredProvCredentialEvents)
    {
        _pCredProvCredentialEvents->Release();
    }
    _pCredProvCredentialEvents = nullptr;
    if (_hMessageWindow) DestroyWindow(_hMessageWindow);
    _hMessageWindow = nullptr;
    UnregisterClassW(L"HomeFaceLogonCredentialEvents", HINST_THISDLL);
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
    if (_fFaceAuthSuccess && !_HasCurrentFaceProof()) _ResetAuthentication();
    LogInfo(L"Credential SetSelected: face=%d, submitting=%d", _fFaceAuthSuccess, _fSubmitting);
    *pbAutoLogon = _fFaceAuthSuccess ? TRUE : FALSE;
    if (!_fFaceAuthSuccess && !_fSubmitting && !_fScanning) _StartHostProcess();
    return S_OK;
}

// Similarly to SetSelected, LogonUI calls this when your tile was selected
// and now no longer is. The most common thing to do here (which we do below)
// is to clear out the password field.
HRESULT CSampleCredential::SetDeselected()
{
    LogInfo(L"Credential SetDeselected: face=%d, submitting=%d", _fFaceAuthSuccess, _fSubmitting);
    _StopHostProcess();
    _ResetAuthentication();

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
        // Keep the 32-bit alpha channel in the provider logo.
        HBITMAP hbmp = static_cast<HBITMAP>(LoadImageW(HINST_THISDLL,
            MAKEINTRESOURCEW(IDB_TILE_IMAGE), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
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
            _ResetAuthentication();
            hr = _StartHostProcess();
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
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    ZeroMemory(pcpcs, sizeof(*pcpcs));
    if (_fSubmitting) return S_OK;
    _StopHostProcess();

    const bool faceMatched = _HasCurrentFaceProof();
    _ResetAuthentication(); // Consume the proof once, including every failure path.

    HRESULT hr = E_UNEXPECTED;

    // PIN lockout applies to PIN attempts, not an independent, successful face match.
    if (!faceMatched && g_nPinAttempts >= 5)
    {
        ULONGLONG now = GetTickCount64();
        if (now < g_lockoutExpiration)
        {
            *ppwszOptionalStatusText = nullptr;
            SHStrDupW(L"PIN の入力に何度も失敗したため、一時的にロックされています。30秒後に再試行してください。", ppwszOptionalStatusText);
            _ClearPin();
            _StartHostProcess();
            return E_ACCESSDENIED;
        }
        else
        {
            g_nPinAttempts = 0;
            g_lockoutExpiration = 0;
        }
    }

    bool authValid = false;

    // A match may arrive while the user is typing. An unfinished PIN must never
    // override that match or count as a failed PIN attempt.
    if (faceMatched)
    {
        authValid = true;
        _ClearPin();
    }
    else if (_rgFieldStrings[SFI_PASSWORD] && _rgFieldStrings[SFI_PASSWORD][0] != L'\0')
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
            _ClearPin();
            _StartHostProcess(); // Keep face authentication available after PIN failure.
            return E_ACCESSDENIED;
        }
    }
    // Case 3: Neither PIN entered nor face auth succeeded (user just clicked submit button with empty PIN field)
    else
    {
        *ppwszOptionalStatusText = nullptr;
        SHStrDupW(L"PIN を入力するか、顔認証を行ってください。", ppwszOptionalStatusText);
        _StartHostProcess();
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

    if (FAILED(hr))
    {
        if (pcpcs->rgbSerialization)
        {
            SecureZeroMemory(pcpcs->rgbSerialization, pcpcs->cbSerialization);
            CoTaskMemFree(pcpcs->rgbSerialization);
        }
        ZeroMemory(pcpcs, sizeof(*pcpcs));
    }
    else if (*pcpgsr == CPGSR_RETURN_CREDENTIAL_FINISHED)
    {
        _fSubmitting = true;
        _ClearPin();
    }

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
    _ResetAuthentication();

    // If we failed the logon, try to erase the password field.
    if (FAILED(HRESULT_FROM_NT(ntsStatus)))
    {
        _ClearPin();
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
    // This state is scoped to the current credential and cleared on all failures/cancellation.
    if (_fFaceAuthSuccess || _fSubmitting)
    {
        return S_OK;
    }
    HRESULT windowHr = _CreateMessageWindow();
    if (FAILED(windowHr)) return windowHr;

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
        DWORD error = GetLastError();
        LogError(L"CreateEventW failed for _hPipeEvent: %lu", error);
        CloseHandle(_hPipe);
        _hPipe = INVALID_HANDLE_VALUE;
        return HRESULT_FROM_WIN32(error);
    }

    // 3. Create Job Object to tie host life to Winlogon/CP
    _hHostJob = CreateJobObjectW(nullptr, nullptr);
    if (_hHostJob == nullptr)
    {
        DWORD error = GetLastError();
        _hHostJob = INVALID_HANDLE_VALUE;
        _StopHostProcess();
        return HRESULT_FROM_WIN32(error);
    }
    else
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(_hHostJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
        {
            DWORD error = GetLastError();
            _StopHostProcess();
            return HRESULT_FROM_WIN32(error);
        }
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
        DWORD error = GetLastError();
        LogError(L"CreateProcessW failed: %lu", error);
        _StopHostProcess();
        _UpdateStatusText(L"ヘルパープロセスの起動に失敗しました");
        return HRESULT_FROM_WIN32(error);
    }

    _hHostProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    if (_hHostJob != INVALID_HANDLE_VALUE)
    {
        if (!AssignProcessToJobObject(_hHostJob, _hHostProcess))
        {
            DWORD error = GetLastError();
            _StopHostProcess();
            return HRESULT_FROM_WIN32(error);
        }
    }

    _fScanning = true;
    _fFaceAuthSuccess = false;

    // 6. Start Named Pipe Read Thread
    _hPipeReadThread = CreateThread(nullptr, 0, _PipeReadThreadProc, this, 0, nullptr);
    if (!_hPipeReadThread)
    {
        DWORD error = GetLastError();
        _hPipeReadThread = INVALID_HANDLE_VALUE;
        _StopHostProcess();
        return HRESULT_FROM_WIN32(error);
    }
    
    _UpdateStatusText(L"カメラを準備しています...");
    return S_OK;
}

HRESULT CSampleCredential::_CreateMessageWindow()
{
    if (_hMessageWindow) return S_OK;
    const wchar_t* className = L"HomeFaceLogonCredentialEvents";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = _MessageWindowProc;
    wc.hInstance = HINST_THISDLL;
    wc.lpszClassName = className;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());
    _hMessageWindow = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, HINST_THISDLL, this);
    return _hMessageWindow ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

LRESULT CALLBACK CSampleCredential::_MessageWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto credential = reinterpret_cast<CSampleCredential*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        credential = static_cast<CSampleCredential*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(credential));
    }
    if (message == WM_HOST_MESSAGE && credential)
    {
        // Queued results from a stopped/replaced camera session must not sign in.
        if (wParam == credential->_scanGeneration && credential->_fScanning)
            credential->_HandlePipeMessage(static_cast<uint16_t>(lParam));
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void CSampleCredential::_ResetAuthentication()
{
    _fFaceAuthSuccess = false;
    _fSubmitting = false;
    _faceMatchedAt = 0;
    if (_pProvider) _pProvider->ResetAutoLogon();
}

bool CSampleCredential::_HasCurrentFaceProof() const
{
    // GetTickCount64 includes suspension: a retained tile cannot reuse a face
    // match after inactivity or sleep. Callback replacement normally takes ms.
    return _fFaceAuthSuccess && GetTickCount64() - _faceMatchedAt < 10000;
}

HRESULT CSampleCredential::_ClearPin()
{
    if (_rgFieldStrings[SFI_PASSWORD])
    {
        SecureZeroMemory(_rgFieldStrings[SFI_PASSWORD], wcslen(_rgFieldStrings[SFI_PASSWORD]) * sizeof(wchar_t));
        CoTaskMemFree(_rgFieldStrings[SFI_PASSWORD]);
        _rgFieldStrings[SFI_PASSWORD] = nullptr;
    }
    HRESULT hr = SHStrDupW(L"", &_rgFieldStrings[SFI_PASSWORD]);
    if (SUCCEEDED(hr) && _pCredProvCredentialEvents)
        _pCredProvCredentialEvents->SetFieldString(this, SFI_PASSWORD, L"");
    return hr;
}

void CSampleCredential::_StopHostProcess()
{
    _fScanning = false;
    ++_scanGeneration;
    if (_hPipeEvent) SetEvent(_hPipeEvent);
    if (_hPipe != INVALID_HANDLE_VALUE) CancelIoEx(_hPipe, nullptr);

    // Worker code never invokes COM or waits for the UI. Keep its pipe, event,
    // nonce and object alive until cancellation has completed and it has exited.
    if (_hPipeReadThread != INVALID_HANDLE_VALUE)
    {
        WaitForSingleObject(_hPipeReadThread, INFINITE);
        CloseHandle(_hPipeReadThread);
        _hPipeReadThread = INVALID_HANDLE_VALUE;
    }
    if (_hHostProcess != INVALID_HANDLE_VALUE)
    {
        TerminateProcess(_hHostProcess, 0);
        CloseHandle(_hHostProcess);
        _hHostProcess = INVALID_HANDLE_VALUE;
    }
    if (_hHostJob != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_hHostJob);
        _hHostJob = INVALID_HANDLE_VALUE;
    }
    if (_hPipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(_hPipe);
        CloseHandle(_hPipe);
        _hPipe = INVALID_HANDLE_VALUE;
    }
    if (_hPipeEvent)
    {
        CloseHandle(_hPipeEvent);
        _hPipeEvent = nullptr;
    }
    _sessionNonceHi = 0;
    _sessionNonceLo = 0;
}

static ULONGLONG AwakeMilliseconds()
{
    ULONGLONG time = 0;
    QueryUnbiasedInterruptTime(&time);
    return time / 10000;
}

DWORD WINAPI CSampleCredential::_PipeReadThreadProc(LPVOID lpParam)
{
    auto pThis = static_cast<CSampleCredential*>(lpParam);
    // These handles and fields remain stable until this worker has been joined.
    const UINT_PTR generation = pThis->_scanGeneration;
    const HANDLE pipe = pThis->_hPipe;
    const HANDLE cancel = pThis->_hPipeEvent;
    const HWND window = pThis->_hMessageWindow;
    auto post = [&](uint16_t type) { PostMessageW(window, WM_HOST_MESSAGE, generation, type); };
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) { post(6); return 0; }

    // Cancellation wins over a simultaneously completed I/O. Always drain an
    // OVERLAPPED operation before its stack buffer or event goes out of scope.
    const ULONGLONG started = AwakeMilliseconds();
    AppConfig config = {};
    LoadAppConfig(&config);
    const ULONGLONG timeout = 65000ULL + static_cast<ULONGLONG>(config.scanTimeoutMs > 0 ? config.scanTimeoutMs : 10000);
    auto complete = [&](OVERLAPPED& ov, DWORD& bytes) -> bool {
        HANDLE handles[] = {cancel, event, pThis->_hHostProcess};
        while (pThis->_fScanning)
        {
            DWORD result = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, 100);
            if (result == WAIT_OBJECT_0 + 1)
                return GetOverlappedResult(pipe, &ov, &bytes, FALSE) != FALSE;
            // If the child exited simultaneously, give its final pipe write
            // a chance to complete; cancellation must still win.
            if (result == WAIT_OBJECT_0 + 2 && WaitForSingleObject(event, 0) == WAIT_OBJECT_0)
                return GetOverlappedResult(pipe, &ov, &bytes, FALSE) != FALSE;
            if (result != WAIT_TIMEOUT || AwakeMilliseconds() - started >= timeout) break;
        }
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        return false;
    };

    OVERLAPPED ov = {};
    ov.hEvent = event;
    DWORD bytes = 0;
    bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
    if (!connected)
    {
        DWORD error = GetLastError();
        connected = error == ERROR_PIPE_CONNECTED || (error == ERROR_IO_PENDING && complete(ov, bytes));
    }
    if (connected)
    {
        struct MessageHeader {
            uint32_t magic;
            uint16_t version;
            uint16_t type;
            uint32_t payloadSize;
            uint64_t sessionNonceHi;
            uint64_t sessionNonceLo;
        };
        while (pThis->_fScanning)
        {
            MessageHeader header = {};
            ResetEvent(event);
            ov = {};
            ov.hEvent = event;
            bytes = 0;
            bool read = ReadFile(pipe, &header, sizeof(header), &bytes, &ov) != FALSE;
            if (!read && GetLastError() == ERROR_IO_PENDING) read = complete(ov, bytes);
            if (!read || bytes != sizeof(header)) break;
            if (!pThis->_fScanning) break;
            if (header.magic != 0x4F4C4648 || header.version != 1 || header.payloadSize != 0 ||
                header.sessionNonceHi != pThis->_sessionNonceHi || header.sessionNonceLo != pThis->_sessionNonceLo)
            {
                LogError(L"Invalid host message; stopping this session.");
                break;
            }
            post(header.type);
            if (header.type >= 4 && header.type <= 7)
            {
                CloseHandle(event);
                return 0;
            }
        }
    }
    if (pThis->_fScanning) post(6);
    CloseHandle(event);
    return 0;
}

void CSampleCredential::_HandlePipeMessage(uint16_t msgType)
{
    // End-to-end latency as the user sees it, including antivirus scans that
    // run before the host process exists (invisible to the host's own log).
    LogInfo(L"Host message type=%u, %llu ms after host launch request",
            msgType, GetTickCount64() - _llCreationTime);
    switch (msgType)
    {
    case 3: // MSG_STATUS
        _UpdateStatusText(L"顔を探しています...");
        break;
    case 4: // MSG_MATCHED
        _StopHostProcess();
        _fFaceAuthSuccess = true;
        _faceMatchedAt = GetTickCount64();
        _UpdateStatusText(L"顔が一致しました。サインインしています...");
        if (_fFaceAuthSuccess && _pProvider) _pProvider->TriggerAutoLogon();
        break;
    case 5: // MSG_NO_MATCH
        _StopHostProcess();
        _UpdateStatusText(L"登録された顔と一致しません");
        break;
    case 6: // MSG_CAMERA_ERROR
        _StopHostProcess();
        _UpdateStatusText(L"カメラを開けませんでした。カメラが他のアプリに使用されていないか確認してください。");
        break;
    case 7: // MSG_MODEL_ERROR
        _StopHostProcess();
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
