//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// CSampleProvider implements ICredentialProvider, which is the main
// interface that logonUI uses to decide which tiles to display.
// In this sample, we will display one tile that uses each of the nine
// available UI controls.

#include <initguid.h>
#include "CSampleProvider.h"
#include "CSampleCredential.h"
#include "guid.h"
#include "log.h"
#include "config.h"

CSampleProvider::CSampleProvider():
    _cRef(1),
    _pCredential(nullptr),
    _pCredProviderUserArray(nullptr)
{
    DllAddRef();
    LogInfo(L"CSampleProvider constructor called.");
}

CSampleProvider::~CSampleProvider()
{
    LogInfo(L"CSampleProvider destructor called.");
    if (_pCredential != nullptr)
    {
        _pCredential->Release();
        _pCredential = nullptr;
    }
    if (_pCredProviderUserArray != nullptr)
    {
        _pCredProviderUserArray->Release();
        _pCredProviderUserArray = nullptr;
    }

    DllRelease();
}

// SetUsageScenario is the provider's cue that it's going to be asked for tiles
// in a subsequent call.
HRESULT CSampleProvider::SetUsageScenario(
    CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
    DWORD /*dwFlags*/)
{
    LogInfo(L"SetUsageScenario called: cpus=%d", cpus);
    HRESULT hr;

    // Decide which scenarios to support here. Returning E_NOTIMPL simply tells the caller
    // that we're not designed for that scenario.
    switch (cpus)
    {
    case CPUS_LOGON:
    case CPUS_UNLOCK_WORKSTATION:
        // The reason why we need _fRecreateEnumeratedCredentials is because ICredentialProviderSetUserArray::SetUserArray() is called after ICredentialProvider::SetUsageScenario(),
        // while we need the ICredentialProviderUserArray during enumeration in ICredentialProvider::GetCredentialCount()
        _cpus = cpus;
        _fRecreateEnumeratedCredentials = true;
        hr = S_OK;
        break;

    case CPUS_CHANGE_PASSWORD:
    case CPUS_CREDUI:
        hr = E_NOTIMPL;
        break;

    default:
        hr = E_INVALIDARG;
        break;
    }

    return hr;
}

// SetSerialization takes the kind of buffer that you would normally return to LogonUI for
// an authentication attempt.  It's the opposite of ICredentialProviderCredential::GetSerialization.
// GetSerialization is implement by a credential and serializes that credential.  Instead,
// SetSerialization takes the serialization and uses it to create a tile.
//
// SetSerialization is called for two main scenarios.  The first scenario is in the credui case
// where it is prepopulating a tile with credentials that the user chose to store in the OS.
// The second situation is in a remote logon case where the remote client may wish to
// prepopulate a tile with a username, or in some cases, completely populate the tile and
// use it to logon without showing any UI.
//
// If you wish to see an example of SetSerialization, please see either the SampleCredentialProvider
// sample or the SampleCredUICredentialProvider sample.  [The logonUI team says, "The original sample that
// this was built on top of didn't have SetSerialization.  And when we decided SetSerialization was
// important enough to have in the sample, it ended up being a non-trivial amount of work to integrate
// it into the main sample.  We felt it was more important to get these samples out to you quickly than to
// hold them in order to do the work to integrate the SetSerialization changes from SampleCredentialProvider
// into this sample.]
HRESULT CSampleProvider::SetSerialization(
    _In_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION const * /*pcpcs*/)
{
    return E_NOTIMPL;
}

// Called by LogonUI to give you a callback.  Providers often use the callback if they
// some event would cause them to need to change the set of tiles that they enumerated.
HRESULT CSampleProvider::Advise(
    _In_ ICredentialProviderEvents * /*pcpe*/,
    _In_ UINT_PTR /*upAdviseContext*/)
{
    return E_NOTIMPL;
}

// Called by LogonUI when the ICredentialProviderEvents callback is no longer valid.
HRESULT CSampleProvider::UnAdvise()
{
    return E_NOTIMPL;
}

// Called by LogonUI to determine the number of fields in your tiles.  This
// does mean that all your tiles must have the same number of fields.
// This number must include both visible and invisible fields. If you want a tile
// to have different fields from the other tiles you enumerate for a given usage
// scenario you must include them all in this count and then hide/show them as desired
// using the field descriptors.
HRESULT CSampleProvider::GetFieldDescriptorCount(
    _Out_ DWORD *pdwCount)
{
    *pdwCount = SFI_NUM_FIELDS;
    return S_OK;
}

// Gets the field descriptor for a particular field.
HRESULT CSampleProvider::GetFieldDescriptorAt(
    DWORD dwIndex,
    _Outptr_result_nullonfailure_ CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR **ppcpfd)
{
    HRESULT hr;
    *ppcpfd = nullptr;

    // Verify dwIndex is a valid field.
    if ((dwIndex < SFI_NUM_FIELDS) && ppcpfd)
    {
        hr = FieldDescriptorCoAllocCopy(s_rgCredProvFieldDescriptors[dwIndex], ppcpfd);
    }
    else
    {
        hr = E_INVALIDARG;
    }

    return hr;
}

// Sets pdwCount to the number of tiles that we wish to show at this time.
static bool _IsProviderEnabled()
{
    HKEY hKey;
    bool enabled = false; // Default is disabled (Enabled = 0)
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\HomeFaceLogon", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD dwType;
        DWORD dwValue = 0;
        DWORD dwSize = sizeof(dwValue);
        if (RegQueryValueExW(hKey, L"Enabled", nullptr, &dwType, reinterpret_cast<BYTE*>(&dwValue), &dwSize) == ERROR_SUCCESS)
        {
            if (dwType == REG_DWORD && dwValue != 0)
            {
                enabled = true;
            }
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

// Sets pdwDefault to the index of the tile which should be used as the default.
// The default tile is the tile which will be shown in the zoomed view by default. If
// more than one provider specifies a default the last used cred prov gets to pick
// the default. If *pbAutoLogonWithDefault is TRUE, LogonUI will immediately call
// GetSerialization on the credential you've specified as the default and will submit
// that credential for authentication without showing any further UI.
HRESULT CSampleProvider::GetCredentialCount(
    _Out_ DWORD *pdwCount,
    _Out_ DWORD *pdwDefault,
    _Out_ BOOL *pbAutoLogonWithDefault)
{
    *pdwDefault = CREDENTIAL_PROVIDER_NO_DEFAULT;
    *pbAutoLogonWithDefault = FALSE;

    if (!_IsProviderEnabled())
    {
        *pdwCount = 0;
        return S_OK;
    }

    if (_fRecreateEnumeratedCredentials)
    {
        _fRecreateEnumeratedCredentials = false;
        _ReleaseEnumeratedCredentials();
        _CreateEnumeratedCredentials();
    }

    *pdwCount = (_pCredential != nullptr) ? 1 : 0;

    return S_OK;
}

// Returns the credential at the index specified by dwIndex. This function is called by logonUI to enumerate
// the tiles.
HRESULT CSampleProvider::GetCredentialAt(
    DWORD dwIndex,
    _Outptr_result_nullonfailure_ ICredentialProviderCredential **ppcpc)
{
    HRESULT hr = E_INVALIDARG;
    *ppcpc = nullptr;

    if ((dwIndex == 0) && ppcpc)
    {
        hr = _pCredential->QueryInterface(IID_PPV_ARGS(ppcpc));
    }
    return hr;
}

// This function will be called by LogonUI after SetUsageScenario succeeds.
// Sets the User Array with the list of users to be enumerated on the logon screen.
HRESULT CSampleProvider::SetUserArray(_In_ ICredentialProviderUserArray *users)
{
    if (_pCredProviderUserArray)
    {
        _pCredProviderUserArray->Release();
    }
    _pCredProviderUserArray = users;
    _pCredProviderUserArray->AddRef();
    return S_OK;
}

void CSampleProvider::_CreateEnumeratedCredentials()
{
    switch (_cpus)
    {
    case CPUS_LOGON:
    case CPUS_UNLOCK_WORKSTATION:
        {
            _EnumerateCredentials();
            break;
        }
    default:
        break;
    }
}

void CSampleProvider::_ReleaseEnumeratedCredentials()
{
    if (_pCredential != nullptr)
    {
        _pCredential->Release();
        _pCredential = nullptr;
    }
}

// Helper to convert GUID to wstring
static std::wstring _GuidToString(const GUID& guid)
{
    wchar_t szGuid[64];
    if (StringFromGUID2(guid, szGuid, 64) > 0)
    {
        return szGuid;
    }
    return L"{GUID-ERROR}";
}

HRESULT CSampleProvider::_EnumerateCredentials()
{
    LogInfo(L"Starting _EnumerateCredentials...");

    AppConfig config;
    bool configLoaded = LoadAppConfig(&config);
    std::wstring targetSid = configLoaded ? config.targetSid : L"";
    LogInfo(L"Config loaded: %s, targetSid: %s", 
            configLoaded ? L"TRUE" : L"FALSE", 
            MaskSid(targetSid).c_str());

    HRESULT hr = E_UNEXPECTED;
    if (_pCredProviderUserArray != nullptr)
    {
        DWORD dwUserCount = 0;
        _pCredProviderUserArray->GetCount(&dwUserCount);
        LogInfo(L"Found %d user(s) in system user array.", dwUserCount);

        ICredentialProviderUser* pTargetUser = nullptr;

        for (DWORD i = 0; i < dwUserCount; ++i)
        {
            ICredentialProviderUser* pUser = nullptr;
            if (SUCCEEDED(_pCredProviderUserArray->GetAt(i, &pUser)))
            {
                PWSTR pszSid = nullptr;
                PWSTR pszUserName = nullptr;
                PWSTR pszDisplayName = nullptr;
                PWSTR pszQualifiedName = nullptr;
                GUID providerId = {};

                pUser->GetSid(&pszSid);
                pUser->GetProviderID(&providerId);
                pUser->GetStringValue(PKEY_Identity_UserName, &pszUserName);
                pUser->GetStringValue(PKEY_Identity_DisplayName, &pszDisplayName);
                pUser->GetStringValue(PKEY_Identity_QualifiedUserName, &pszQualifiedName);

                std::wstring wsSid = pszSid ? pszSid : L"";
                std::wstring wsUserName = pszUserName ? pszUserName : L"";
                std::wstring wsDisplayName = pszDisplayName ? pszDisplayName : L"";
                std::wstring wsQualifiedName = pszQualifiedName ? pszQualifiedName : L"";

                LogInfo(L"User[%d]: DisplayName='%ls', UserName='%ls', QualifiedName='%ls', SID='%ls', ProviderID='%ls'",
                        i,
                        wsDisplayName.c_str(),
                        wsUserName.c_str(),
                        MaskQualifiedUserName(wsQualifiedName).c_str(),
                        MaskSid(wsSid).c_str(),
                        _GuidToString(providerId).c_str());

                if (!targetSid.empty() && _wcsicmp(wsSid.c_str(), targetSid.c_str()) == 0)
                {
                    LogInfo(L"Found matching user at index %d.", i);
                    pTargetUser = pUser;
                    pTargetUser->AddRef();
                }

                if (pszSid) CoTaskMemFree(pszSid);
                if (pszUserName) CoTaskMemFree(pszUserName);
                if (pszDisplayName) CoTaskMemFree(pszDisplayName);
                if (pszQualifiedName) CoTaskMemFree(pszQualifiedName);

                pUser->Release();
            }
        }

        if (pTargetUser != nullptr)
        {
            _pCredential = new(std::nothrow) CSampleCredential();
            if (_pCredential != nullptr)
            {
                hr = _pCredential->Initialize(_cpus, s_rgCredProvFieldDescriptors, s_rgFieldStatePairs, pTargetUser);
                if (FAILED(hr))
                {
                    LogError(L"CSampleCredential::Initialize failed: HRESULT=0x%08X", hr);
                    _pCredential->Release();
                    _pCredential = nullptr;
                }
                else
                {
                    LogInfo(L"CSampleCredential initialized successfully.");
                }
            }
            else
            {
                hr = E_OUTOFMEMORY;
                LogError(L"Failed to allocate CSampleCredential.");
            }
            pTargetUser->Release();
        }
        else
        {
            LogInfo(L"No user in user array matched the targetSid.");
            hr = S_OK;
        }
    }
    return hr;
}

// Boilerplate code to create our provider.
HRESULT CSample_CreateInstance(_In_ REFIID riid, _Outptr_ void **ppv)
{
    HRESULT hr;
    CSampleProvider *pProvider = new(std::nothrow) CSampleProvider();
    if (pProvider)
    {
        hr = pProvider->QueryInterface(riid, ppv);
        pProvider->Release();
    }
    else
    {
        hr = E_OUTOFMEMORY;
    }
    return hr;
}
