#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include "../src/provider/config.h"
#include "../src/setup/credential_input.h"
#include "../src/common/camera_preference.h"

static int pinChecks = 0;
static int hostLaunches = 0;
static bool decryptFails = false;
static BOOL WINAPI TestCreateProcessW(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION)
{
    ++hostLaunches;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return FALSE; // Unit tests never start the installed camera host.
}
static HRESULT TestRetrieveNegotiateAuthPackage(ULONG* package);
#define CreateProcessW TestCreateProcessW
#define RetrieveNegotiateAuthPackage TestRetrieveNegotiateAuthPackage
#include "../src/provider/CSampleCredential.cpp"
#undef CreateProcessW
#undef RetrieveNegotiateAuthPackage

HINSTANCE g_hinst = GetModuleHandleW(nullptr);
void DllAddRef() {}
void DllRelease() {}
void LogInit() {}
void LogShutdown() {}
void LogInfo(const wchar_t*, ...) {}
void LogError(const wchar_t*, ...) {}
std::wstring MaskQualifiedUserName(const std::wstring&) { return L"masked"; }
std::wstring MaskSid(const std::wstring&) { return L"masked"; }
bool LoadAppConfig(AppConfig* config) { *config = {}; config->scanTimeoutMs = 10000; return true; }
bool VerifyPin(PCWSTR pin) { ++pinChecks; return wcscmp(pin, L"1234") == 0; }
bool LoadAndDecryptPassword(PWSTR* password) {
    if (decryptFails) return false;
    return SUCCEEDED(SHStrDupW(L"Dummy-Regression-Password", password));
}
static HRESULT TestRetrieveNegotiateAuthPackage(ULONG* package) { *package = 0x55; return S_OK; }

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class CredentialTestAccess {
public:
    static CSampleCredential* create() {
        auto c = new CSampleCredential;
        c->_cpus = CPUS_LOGON;
        c->_fIsLocalUser = true;
        SHStrDupW(L".\\DummyRegressionUser", &c->_pszQualifiedUserName);
        c->_rgCredProvFieldDescriptors[SFI_PASSWORD].cpft = CPFT_PASSWORD_TEXT;
        SHStrDupW(L"", &c->_rgFieldStrings[SFI_PASSWORD]);
        return c;
    }
    static void match(CSampleCredential* c) { c->_fFaceAuthSuccess = true; c->_faceMatchedAt = GetTickCount64(); }
    static void expireMatch(CSampleCredential* c) { c->_faceMatchedAt = GetTickCount64() - 10001; }
    static bool pending(CSampleCredential* c) { return c->_fFaceAuthSuccess || c->_fSubmitting; }
    static CSampleProvider* attachProvider(CSampleCredential* c) {
        auto p = new CSampleProvider;
        p->_pCredential = c;
        c->AddRef();
        c->SetProvider(p);
        return p;
    }
    static bool autoLogon(CSampleProvider* p) { return p->_fAutoLogon; }
    static void nonLocal(CSampleCredential* c) {
        c->_fIsLocalUser = false;
        CoTaskMemFree(c->_pszQualifiedUserName);
        SHStrDupW(L"MicrosoftAccount\\" L"dummy@example.invalid", &c->_pszQualifiedUserName);
    }
    static void staleMessage(CSampleCredential* c) {
        check(SUCCEEDED(c->_CreateMessageWindow()), "event window failed");
        c->_fScanning = true;
        PostMessageW(c->_hMessageWindow, WM_HOST_MESSAGE, c->_scanGeneration, 4);
        c->_StopHostProcess();
        MSG message;
        while (PeekMessageW(&message, c->_hMessageWindow, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        check(!c->_fFaceAuthSuccess, "cancelled result was accepted");
    }
    static void queuedMatch(CSampleCredential* c) {
        check(SUCCEEDED(c->_CreateMessageWindow()), "event window failed");
        c->_fScanning = true;
        const UINT_PTR generation = c->_scanGeneration;
        std::thread producer([=] { PostMessageW(c->_hMessageWindow, WM_HOST_MESSAGE, generation, 4); });
        producer.join();
        check(!c->_fFaceAuthSuccess, "worker modified credential before UI delivery");
        MSG message;
        while (PeekMessageW(&message, c->_hMessageWindow, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        check(c->_fFaceAuthSuccess, "current match was not delivered");
    }
    static void cancelPendingRead(CSampleCredential* c, bool connected) {
        check(SUCCEEDED(c->_CreateMessageWindow()), "event window failed");
        wchar_t name[128];
        swprintf_s(name, L"\\\\.\\pipe\\HflRegression-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
        c->_hPipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 1024, 1024, 0, nullptr);
        check(c->_hPipe != INVALID_HANDLE_VALUE, "test pipe failed");
        c->_hPipeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        c->_hHostProcess = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
        c->_fScanning = true;
        c->_hPipeReadThread = CreateThread(nullptr, 0, c->_PipeReadThreadProc, c, 0, nullptr);
        HANDLE client = INVALID_HANDLE_VALUE;
        if (connected) {
            for (int i = 0; i < 100 && client == INVALID_HANDLE_VALUE; ++i) {
                client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (client == INVALID_HANDLE_VALUE) Sleep(10);
            }
            check(client != INVALID_HANDLE_VALUE, "test client failed");
        }
        Sleep(20);
        auto started = GetTickCount64();
        c->_StopHostProcess();
        check(GetTickCount64() - started < 1000, "pipe cancellation did not finish promptly");
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    }
    static void adviseDuringScan(CSampleCredential* c, ICredentialProviderCredentialEvents* events) {
        check(SUCCEEDED(c->_CreateMessageWindow()), "event window failed");
        c->_fScanning = true;
        const UINT_PTR generation = c->_scanGeneration;
        c->Advise(events); // LogonUI may (re)issue Advise after SetSelected started the camera.
        check(c->_fScanning && c->_scanGeneration == generation, "Advise stopped the running camera session");
        PostMessageW(c->_hMessageWindow, WM_HOST_MESSAGE, generation, 4);
        MSG message;
        while (PeekMessageW(&message, c->_hMessageWindow, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        check(c->_fFaceAuthSuccess, "match after Advise was not delivered");
    }
    static void childExit(CSampleCredential* c) {
        check(SUCCEEDED(c->_CreateMessageWindow()), "event window failed");
        wchar_t name[128];
        swprintf_s(name, L"\\\\.\\pipe\\HflExitRegression-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
        c->_hPipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 1024, 1024, 0, nullptr);
        check(c->_hPipe != INVALID_HANDLE_VALUE, "test pipe failed");
        c->_hPipeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        c->_sessionNonceHi = 11; c->_sessionNonceLo = 22;
        wchar_t executable[MAX_PATH]; GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --pipe-client " + name;
        STARTUPINFOW startup = {sizeof(startup)}; PROCESS_INFORMATION process = {};
        check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "test child failed");
        CloseHandle(process.hThread);
        c->_hHostProcess = process.hProcess;
        c->_fScanning = true;
        c->_hPipeReadThread = CreateThread(nullptr, 0, c->_PipeReadThreadProc, c, 0, nullptr);
        check(WaitForSingleObject(c->_hPipeReadThread, 3000) == WAIT_OBJECT_0, "final pipe message was not read");
        MSG message;
        while (PeekMessageW(&message, c->_hMessageWindow, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        check(c->_fFaceAuthSuccess, "final match was lost when child exited");
    }
};

// Minimal LogonUI callback: only identity matters for Advise ordering tests.
class TestEvents : public ICredentialProviderCredentialEvents {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ICredentialProviderCredentialEvents)) { *ppv = this; return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 1; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP SetFieldState(ICredentialProviderCredential*, DWORD, CREDENTIAL_PROVIDER_FIELD_STATE) override { return S_OK; }
    IFACEMETHODIMP SetFieldInteractiveState(ICredentialProviderCredential*, DWORD, CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE) override { return S_OK; }
    IFACEMETHODIMP SetFieldString(ICredentialProviderCredential*, DWORD, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP SetFieldCheckbox(ICredentialProviderCredential*, DWORD, BOOL, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP SetFieldBitmap(ICredentialProviderCredential*, DWORD, HBITMAP) override { return S_OK; }
    IFACEMETHODIMP SetFieldComboBoxSelectedItem(ICredentialProviderCredential*, DWORD, DWORD) override { return S_OK; }
    IFACEMETHODIMP DeleteFieldComboBoxItem(ICredentialProviderCredential*, DWORD, DWORD) override { return S_OK; }
    IFACEMETHODIMP AppendFieldComboBoxItem(ICredentialProviderCredential*, DWORD, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP SetFieldSubmitButton(ICredentialProviderCredential*, DWORD, DWORD) override { return S_OK; }
    IFACEMETHODIMP OnCreatingWindow(HWND* hwnd) override { *hwnd = nullptr; return S_OK; }
};

static HRESULT serialize(CSampleCredential* c, bool expectFinished) {
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION buffer;
    PWSTR text = nullptr;
    CREDENTIAL_PROVIDER_STATUS_ICON icon;
    auto hr = c->GetSerialization(&response, &buffer, &text, &icon);
    if (expectFinished) {
        check(SUCCEEDED(hr) && response == CPGSR_RETURN_CREDENTIAL_FINISHED && buffer.rgbSerialization,
            "credentials were not serialized");
    } else check(response == CPGSR_NO_CREDENTIAL_NOT_FINISHED && !buffer.rgbSerialization, "unexpected credentials");
    if (buffer.rgbSerialization) { SecureZeroMemory(buffer.rgbSerialization, buffer.cbSerialization); CoTaskMemFree(buffer.rgbSerialization); }
    CoTaskMemFree(text);
    return hr;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 3 && wcscmp(argv[1], L"--pipe-client") == 0) {
        HANDLE pipe = CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return 1;
        struct Header { uint32_t magic; uint16_t version; uint16_t type; uint32_t size; uint64_t hi; uint64_t lo; };
        Header header = {0x4F4C4648, 1, 4, 0, 11, 22};
        DWORD written = 0;
        BOOL ok = WriteFile(pipe, &header, sizeof(header), &written, nullptr);
        CloseHandle(pipe);
        return ok && written == sizeof(header) ? 0 : 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        for (auto input : { L"1", L"wrong-pin", L"1234", L"" }) {
            auto c = CredentialTestAccess::create();
            c->SetStringValue(SFI_PASSWORD, input);
            CredentialTestAccess::queuedMatch(c);
            const int before = pinChecks;
            serialize(c, true);
            check(pinChecks == before, "PIN overrode a face match");
            c->Release();
        }
        std::cout << "PASS face match with partial, invalid, valid and empty PIN\n";
        auto refreshed = CredentialTestAccess::create();
        auto provider = CredentialTestAccess::attachProvider(refreshed);
        refreshed->SetStringValue(SFI_PASSWORD, L"1");
        CredentialTestAccess::queuedMatch(refreshed);
        check(CredentialTestAccess::autoLogon(provider), "face match did not request auto logon");
        refreshed->UnAdvise(); // LogonUI replaces callbacks during asynchronous re-enumeration.
        check(CredentialTestAccess::autoLogon(provider) && CredentialTestAccess::pending(refreshed),
            "callback replacement consumed auto logon proof");
        BOOL refreshedAutoLogon = FALSE;
        const int launchesBeforeRefresh = hostLaunches;
        refreshed->SetSelected(&refreshedAutoLogon);
        check(refreshedAutoLogon && hostLaunches == launchesBeforeRefresh,
            "refreshed tile restarted camera instead of submitting its face proof");
        serialize(refreshed, true);
        refreshed->UnAdvise();
        check(CredentialTestAccess::pending(refreshed), "callback replacement cancelled in-flight submission");
        check(serialize(refreshed, false) == S_OK, "callback replacement permitted duplicate submission");
        provider->Release();
        check(!CredentialTestAccess::pending(refreshed), "provider teardown retained an orphaned proof");
        refreshed->Release();
        std::cout << "PASS auto logon and submission survive callback replacement; teardown cancels proof\n";
        refreshed = CredentialTestAccess::create();
        CredentialTestAccess::match(refreshed);
        refreshed->UnAdvise();
        CredentialTestAccess::expireMatch(refreshed);
        refreshedAutoLogon = TRUE;
        refreshed->SetSelected(&refreshedAutoLogon);
        check(!refreshedAutoLogon && !CredentialTestAccess::pending(refreshed), "expired proof auto logged on");
        refreshed->Release();
        refreshed = CredentialTestAccess::create();
        CredentialTestAccess::match(refreshed);
        CredentialTestAccess::expireMatch(refreshed);
        check(serialize(refreshed, false) == E_ACCESSDENIED, "serialization accepted expired proof");
        refreshed->Release();
        std::cout << "PASS old face proof expires before selection or serialization\n";
        auto msa = CredentialTestAccess::create();
        CredentialTestAccess::nonLocal(msa);
        msa->SetStringValue(SFI_PASSWORD, L"1");
        CredentialTestAccess::match(msa);
        serialize(msa, true);
        BOOL autoLogon = FALSE;
        int launchesBefore = hostLaunches;
        msa->SetSelected(&autoLogon);
        check(hostLaunches == launchesBefore, "tile reselection launched another host during submission");
        check(serialize(msa, false) == S_OK && CredentialTestAccess::pending(msa), "duplicate submission changed in-flight state");
        msa->Release();
        std::cout << "PASS Microsoft account packaging and reselection during submission\n";
        g_nPinAttempts = 5; g_lockoutExpiration = GetTickCount64() + 30000;
        auto c = CredentialTestAccess::create();
        c->SetStringValue(SFI_PASSWORD, L"wrong");
        CredentialTestAccess::match(c);
        serialize(c, true);
        check(g_nPinAttempts == 5, "face path changed PIN lockout");
        c->Release();
        c = CredentialTestAccess::create();
        c->SetStringValue(SFI_PASSWORD, L"1234");
        check(serialize(c, false) == E_ACCESSDENIED, "PIN bypassed lockout");
        c->Release();
        std::cout << "PASS PIN lockout remains enforced; face match remains independent\n";
        g_nPinAttempts = 0; g_lockoutExpiration = 0;
        c = CredentialTestAccess::create();
        decryptFails = true;
        CredentialTestAccess::match(c);
        check(serialize(c, false) == E_FAIL && !CredentialTestAccess::pending(c), "decryption failure retained authentication state");
        decryptFails = false;
        c->SetStringValue(SFI_PASSWORD, L"1234");
        serialize(c, true);
        PWSTR text = nullptr; CREDENTIAL_PROVIDER_STATUS_ICON icon;
        c->ReportResult(STATUS_LOGON_FAILURE, STATUS_SUCCESS, &text, &icon);
        CoTaskMemFree(text);
        check(!CredentialTestAccess::pending(c), "Windows logon rejection retained state");
        c->Release();
        std::cout << "PASS serialization failure and Windows rejection recover\n";
        c = CredentialTestAccess::create();
        CredentialTestAccess::staleMessage(c);
        CredentialTestAccess::match(c);
        c->SetDeselected();
        check(!CredentialTestAccess::pending(c), "deselection retained match");
        c->Release();
        std::cout << "PASS cancelled session cannot deliver a queued face match\n";
        for (bool connected : {false, true}) {
            c = CredentialTestAccess::create();
            CredentialTestAccess::cancelPendingRead(c, connected);
            c->Release();
        }
        std::cout << "PASS cancellation during pipe connection and read\n";
        for (int i = 0; i < 10; ++i) {
            c = CredentialTestAccess::create();
            CredentialTestAccess::childExit(c);
            c->Release();
        }
        std::cout << "PASS final match survives immediate child exit (10 iterations)\n";
        TestEvents events;
        c = CredentialTestAccess::create();
        CredentialTestAccess::adviseDuringScan(c, &events);
        c->Release();
        std::cout << "PASS Advise during an active scan keeps the camera session and its result\n";
        {
            using namespace hfl_camera;
            Preference pref; // first run: MSMF by default
            pref = NextPreference(pref, true, Backend::DSHOW); // camera busy once
            check(pref.backend == Backend::MSMF && pref.streak == 1, "one fallback demoted MSMF");
            pref = NextPreference(pref, true, Backend::MSMF);
            check(pref.backend == Backend::MSMF && pref.streak == 0, "MSMF success did not reset fallbacks");
            pref = NextPreference(NextPreference(pref, true, Backend::DSHOW), true, Backend::DSHOW);
            check(pref.backend == Backend::DSHOW && pref.streak == 0, "repeated MSMF failure did not prefer DSHOW");
            for (int i = 1; i < kDshowSessionsBeforeRetry; ++i) pref = NextPreference(pref, true, Backend::DSHOW);
            check(pref.backend == Backend::DSHOW, "DSHOW preference expired early");
            pref = NextPreference(pref, true, Backend::DSHOW);
            check(pref.backend == Backend::MSMF, "MSMF was never retried");
            check(NextPreference(pref, true, Backend::DSHOW).backend == Backend::DSHOW,
                "MSMF retry on a DSHOW-only device was not demoted after one slow session");
            pref = Preference{};
            check(NextPreference(pref, false, Backend::DSHOW).backend == Backend::DSHOW,
                "unmappable camera must use DSHOW");
        }
        std::cout << "PASS camera backend preference survives transient contention\n";
        for (auto bytes : {std::string("ascii\n"), std::string("\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\xF0\x9F\x94\x91\r\n")}) {
            std::istringstream stream(bytes); std::wstring value;
            check(ReadUtf8CredentialLine(stream, value) && !value.empty(), "UTF-8 input rejected");
            if (bytes[0] != 'a') check(value == L"テスト🔑", "UTF-8 password changed");
        }
        std::istringstream invalid(std::string("\xff\n")); std::wstring value;
        check(!ReadUtf8CredentialLine(invalid, value), "invalid UTF-8 accepted");
        std::istringstream nul(std::string("abc\0def\n", 8));
        check(!ReadUtf8CredentialLine(nul, value), "embedded NUL accepted");
        std::cout << "PASS UTF-8 credential input and invalid input rejection\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n'; return 1;
    }
    CoUninitialize();
    return 0;
}
