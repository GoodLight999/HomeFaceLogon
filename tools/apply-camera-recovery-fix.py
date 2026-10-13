#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8')

def write(rel, text):
    (ROOT / rel).write_text(text, encoding='utf-8', newline='\n')

def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected one match, found {count}')
    return text.replace(old, new, 1)

def replace_span(text, start, end, replacement, label):
    i = text.find(start)
    if i < 0:
        raise RuntimeError(f'{label}: start not found')
    j = text.find(end, i)
    if j < 0:
        raise RuntimeError(f'{label}: end not found')
    return text[:i] + replacement + text[j:]

# Host: eliminate detached camera workers. A watchdog terminates the isolated
# host process if a driver call never returns, which guarantees device release.
host_path = 'src/host/main.cpp'
host = read(host_path)
host = replace_once(host, '''    ~ComApartment()
    {
        if (hr == S_OK)
        {
            CoUninitialize();
        }
    }''', '''    ~ComApartment()
    {
        if (SUCCEEDED(hr))
        {
            CoUninitialize();
        }
    }''', 'host COM lifetime')

host_camera = r'''static ULONGLONG GetAwakeMilliseconds()
{
    ULONGLONG unbiased100ns = 0;
    if (QueryUnbiasedInterruptTime(&unbiased100ns))
    {
        return unbiased100ns / 10000ULL;
    }
    return GetTickCount64();
}

// Camera driver calls can block inside DirectShow/MSMF and cannot be safely
// cancelled from another thread. FaceLogonHost is an isolated helper process,
// so a watchdog terminates the whole process on a genuine awake-time timeout.
// No detached thread can survive and keep the camera device locked.
static bool OpenCameraWithRetry(cv::VideoCapture& cap, int cameraIndex, int& width, int& height)
{
    const DWORD OVERALL_TIMEOUT_MS = 60000;
    const int MAX_RETRIES = 8;
    const int RETRY_DELAY_MS = 1500;

    std::atomic<bool> finished{false};
    const ULONGLONG startAwakeMs = GetAwakeMilliseconds();
    std::thread watchdog([&finished, startAwakeMs]()
    {
        while (!finished.load())
        {
            Sleep(100);
            if (GetAwakeMilliseconds() - startAwakeMs >= OVERALL_TIMEOUT_MS)
            {
                HostLog("ERROR",
                        "Camera initialization exceeded %lu ms of awake time. "
                        "Terminating isolated host process to release the device.",
                        OVERALL_TIMEOUT_MS);
                TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
                return;
            }
        }
    });

    bool success = false;
    for (int attempt = 1; attempt <= MAX_RETRIES && g_running; ++attempt)
    {
        const bool allowFallback = (attempt >= MAX_RETRIES - 2);
        HostLog("INFO", "Opening camera index %d (attempt %d/%d, allowFallback=%s)...",
                cameraIndex, attempt, MAX_RETRIES,
                allowFallback ? "true" : "false");

        ULONGLONG backendStart = GetAwakeMilliseconds();
        cap.open(cameraIndex, cv::CAP_DSHOW);
        HostLog("INFO", "CAP_DSHOW finished: opened=%d, awakeElapsed=%llums",
                cap.isOpened(), GetAwakeMilliseconds() - backendStart);

        if (!cap.isOpened() && allowFallback)
        {
            backendStart = GetAwakeMilliseconds();
            cap.open(cameraIndex, cv::CAP_MSMF);
            HostLog("INFO", "CAP_MSMF finished: opened=%d, awakeElapsed=%llums",
                    cap.isOpened(), GetAwakeMilliseconds() - backendStart);
        }

        if (!cap.isOpened() && attempt == MAX_RETRIES)
        {
            backendStart = GetAwakeMilliseconds();
            cap.open(cameraIndex);
            HostLog("INFO", "Default backend finished: opened=%d, awakeElapsed=%llums",
                    cap.isOpened(), GetAwakeMilliseconds() - backendStart);
        }

        if (cap.isOpened())
        {
            ConfigureCameraProperties(cap);
            width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
            height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
            if (width <= 0 || height <= 0)
            {
                width = kCameraWidth;
                height = kCameraHeight;
            }

            const int warmAttempts = (attempt < MAX_RETRIES) ? 12 : 40;
            if (WarmUpCamera(cap, warmAttempts, 100))
            {
                HostLog("INFO", "Camera opened and streaming successfully (%dx%d).", width, height);
                success = true;
                break;
            }

            HostLog("WARNING", "Camera opened but did not stream. Releasing before retry.");
            cap.release();
        }

        if (attempt < MAX_RETRIES)
        {
            Sleep(RETRY_DELAY_MS);
        }
    }

    finished.store(true);
    if (watchdog.joinable())
    {
        watchdog.join();
    }

    if (!success)
    {
        HostLog("ERROR", "Failed to open camera device after all retries.");
    }
    return success;
}

'''
host = replace_span(host, 'struct CameraOpenResult', 'static bool LoadHostConfig', host_camera, 'host camera lifecycle')
write(host_path, host)

# Setup helper: enumerate camera friendly names and validate the selected index.
setup_path = 'src/setup/main.cpp'
setup = read(setup_path)
setup = replace_once(setup, '#include <windows.h>\n', '#include <windows.h>\n#include <dshow.h>\n', 'DirectShow include')
setup = replace_once(setup, '#pragma comment(lib, "Bcrypt.lib")\n', '#pragma comment(lib, "Bcrypt.lib")\n#pragma comment(lib, "Strmiids.lib")\n#pragma comment(lib, "Ole32.lib")\n#pragma comment(lib, "OleAut32.lib")\n', 'DirectShow libs')
setup = replace_once(setup, '''    ~ComApartment()
    {
        if (hr == S_OK)
        {
            CoUninitialize();
        }
    }''', '''    ~ComApartment()
    {
        if (SUCCEEDED(hr))
        {
            CoUninitialize();
        }
    }''', 'setup COM lifetime')

camera_enum = r'''struct CameraDeviceInfo
{
    int index;
    std::wstring name;
};

static std::vector<CameraDeviceInfo> EnumerateCameraDevices()
{
    std::vector<CameraDeviceInfo> devices;
    ComApartment com;
    if (!com.Ok()) return devices;

    ICreateDevEnum* deviceEnumerator = nullptr;
    IEnumMoniker* monikerEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&deviceEnumerator));
    if (FAILED(hr) || deviceEnumerator == nullptr) return devices;

    hr = deviceEnumerator->CreateClassEnumerator(
        CLSID_VideoInputDeviceCategory, &monikerEnumerator, 0);
    if (hr != S_OK || monikerEnumerator == nullptr)
    {
        deviceEnumerator->Release();
        return devices;
    }

    IMoniker* moniker = nullptr;
    ULONG fetched = 0;
    int index = 0;
    while (monikerEnumerator->Next(1, &moniker, &fetched) == S_OK)
    {
        std::wstring name = L"Camera " + std::to_wstring(index);
        IPropertyBag* propertyBag = nullptr;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr,
                                             IID_PPV_ARGS(&propertyBag))) &&
            propertyBag != nullptr)
        {
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(propertyBag->Read(L"FriendlyName", &value, nullptr)) &&
                value.vt == VT_BSTR && value.bstrVal != nullptr)
            {
                name = value.bstrVal;
            }
            VariantClear(&value);
            propertyBag->Release();
        }
        for (wchar_t& ch : name)
        {
            if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
        }
        devices.push_back({index, name});
        ++index;
        moniker->Release();
    }

    monikerEnumerator->Release();
    deviceEnumerator->Release();
    return devices;
}

static bool IsCameraIndexAvailable(int cameraIndex, std::wstring* friendlyName = nullptr)
{
    for (const auto& device : EnumerateCameraDevices())
    {
        if (device.index == cameraIndex)
        {
            if (friendlyName != nullptr) *friendlyName = device.name;
            return true;
        }
    }
    return false;
}

static void PrintCameraDevices()
{
    for (const auto& device : EnumerateCameraDevices())
    {
        std::wcout << device.index << L"\t" << device.name << std::endl;
    }
}

'''
setup = replace_once(setup, 'static void ConfigureCameraProperties(cv::VideoCapture& cap)\n', camera_enum + 'static void ConfigureCameraProperties(cv::VideoCapture& cap)\n', 'camera enumeration')
setup = replace_once(setup, '''static bool OpenAndConfigureCamera(cv::VideoCapture& cap, int cameraIndex, int& width, int& height)
{
    const int MAX_RETRIES = 3;''', '''static bool OpenAndConfigureCamera(cv::VideoCapture& cap, int cameraIndex, int& width, int& height)
{
    std::wstring friendlyName;
    if (!IsCameraIndexAvailable(cameraIndex, &friendlyName))
    {
        std::wcerr << L"Error: Camera index " << cameraIndex
                   << L" is not present. Refresh the camera list and choose a named device."
                   << std::endl;
        return false;
    }
    std::wcout << L"Selected camera: [" << cameraIndex << L"] "
               << friendlyName << std::endl;

    const int MAX_RETRIES = 3;''', 'camera index validation')

atomic_helper = r'''template <typename Writer>
static bool WriteFileAtomically(const std::wstring& finalPath, Writer writer)
{
    const std::wstring tempPath = finalPath + L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream file(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        writer(file);
        file.flush();
        if (!file.good())
        {
            file.close();
            DeleteFileW(tempPath.c_str());
            return false;
        }
    }
    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

'''
setup = replace_once(setup, 'bool SaveSecret(const std::wstring& password)\n', atomic_helper + 'bool SaveSecret(const std::wstring& password)\n', 'atomic writer')

save_secret = r'''bool SaveSecret(const std::wstring& password)
{
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(password.c_str()));
    input.cbData = static_cast<DWORD>((password.length() + 1) * sizeof(wchar_t));
    DATA_BLOB output = {0};
    if (!CryptProtectData(&input, L"HomeFaceLogon MSA password", nullptr, nullptr,
                          nullptr, CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
                          &output))
    {
        std::wcerr << L"CryptProtectData failed: " << GetLastError() << std::endl;
        return false;
    }

    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"secret.bin");

    SecretFileHeader header{};
    header.magic = HFLO_MAGIC;
    header.schemaVersion = 1;
    header.protectedBlobSize = output.cbData;
    const bool ok = WriteFileAtomically(path, [&](std::ofstream& file)
    {
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    });
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return ok;
}

'''
setup = replace_span(setup, 'bool SaveSecret(const std::wstring& password)', 'struct PinFileHeader', save_secret, 'SaveSecret')

save_pin = r'''bool SavePin(const std::wstring& pin)
{
    if (pin.empty()) return false;
    PinFileHeader header = {};
    header.magic = HFLP_MAGIC;
    header.schemaVersion = 1;
    header.iterations = 100000;
    NTSTATUS status = BCryptGenRandom(nullptr, header.salt, sizeof(header.salt),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) return false;

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pin.c_str(), -1,
                                      nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return false;
    std::vector<char> pinUtf8(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, pin.c_str(), -1,
                        pinUtf8.data(), utf8Len, nullptr, nullptr);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                          nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status))
    {
        SecureZeroMemory(pinUtf8.data(), pinUtf8.size());
        return false;
    }
    uint8_t derivedKey[PIN_HASH_SIZE] = {};
    status = BCryptDeriveKeyPBKDF2(hAlg,
        reinterpret_cast<PUCHAR>(pinUtf8.data()), static_cast<ULONG>(utf8Len - 1),
        header.salt, sizeof(header.salt), header.iterations,
        derivedKey, PIN_HASH_SIZE, 0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    SecureZeroMemory(pinUtf8.data(), pinUtf8.size());
    if (!BCRYPT_SUCCESS(status)) return false;

    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        SecureZeroMemory(derivedKey, sizeof(derivedKey));
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"pin.bin");
    const bool ok = WriteFileAtomically(path, [&](std::ofstream& file)
    {
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(derivedKey), PIN_HASH_SIZE);
    });
    SecureZeroMemory(derivedKey, sizeof(derivedKey));
    return ok;
}

'''
setup = replace_span(setup, 'bool SavePin(const std::wstring& pin)', 'bool SaveConfig', save_pin, 'SavePin')

save_config = r'''bool SaveConfig(const std::wstring& sid, int cameraIndex)
{
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path))) return false;
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"config.json");

    const std::string utf8Sid = utf16_to_utf8(sid);
    std::string content;
    content += "{\n";
    content += "  \"schemaVersion\": 1,\n";
    content += "  \"enabled\": false,\n";
    content += "  \"targetSid\": \"" + utf8Sid + "\",\n";
    content += "  \"matchThreshold\": 0.363,\n";
    content += "  \"requiredMatches\": 3,\n";
    content += "  \"windowSize\": 5,\n";
    content += "  \"scanTimeoutMs\": 10000,\n";
    content += "  \"livenessEnabled\": false,\n";
    content += "  \"cameraIndex\": " + std::to_string(cameraIndex) + "\n";
    content += "}\n";
    return WriteFileAtomically(path, [&](std::ofstream& file)
    {
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    });
}

'''
setup = replace_span(setup, 'bool SaveConfig(const std::wstring& sid)', 'bool SaveFaceTemplate', save_config, 'SaveConfig')

save_face = r'''bool SaveFaceTemplate(const std::vector<float>& featureVec)
{
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<float*>(featureVec.data()));
    input.cbData = static_cast<DWORD>(featureVec.size() * sizeof(float));
    DATA_BLOB output = {0};
    if (!CryptProtectData(&input, L"HomeFaceLogon Face Template", nullptr, nullptr,
                          nullptr, CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
                          &output)) return false;

    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        LocalFree(output.pbData);
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"face.bin");
    FaceFileHeader header{};
    header.magic = HFLF_MAGIC;
    header.schemaVersion = 1;
    header.protectedBlobSize = output.cbData;
    const bool ok = WriteFileAtomically(path, [&](std::ofstream& file)
    {
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    });
    LocalFree(output.pbData);
    return ok;
}

'''
setup = replace_span(setup, 'bool SaveFaceTemplate(const std::vector<float>& featureVec)', 'bool LoadFaceTemplate', save_face, 'SaveFaceTemplate')

probe_function = r'''bool ProbeCamera(int cameraIndex)
{
    std::wstring friendlyName;
    if (!IsCameraIndexAvailable(cameraIndex, &friendlyName))
    {
        std::wcerr << L"Camera index " << cameraIndex << L" is not present." << std::endl;
        return false;
    }
    ComApartment com;
    if (!com.Ok()) return false;
    cv::VideoCapture cap;
    int width = kCameraWidth;
    int height = kCameraHeight;
    if (!OpenAndConfigureCamera(cap, cameraIndex, width, height)) return false;
    cv::Mat frame;
    const bool ok = cap.read(frame) && !frame.empty();
    cap.release();
    cv::destroyAllWindows();
    if (!ok)
    {
        std::wcerr << L"Camera opened but produced no frame." << std::endl;
        return false;
    }
    std::wcout << L"Camera probe succeeded: [" << cameraIndex << L"] "
               << friendlyName << L" (" << width << L"x" << height << L")" << std::endl;
    return true;
}

'''
setup = replace_once(setup, 'bool RunDiagnostics()\n', probe_function + 'bool RunDiagnostics()\n', 'ProbeCamera')
setup = replace_once(setup, '''    bool enroll = false;
    bool verify = false;
    bool test = false;
    int cameraIndex = 0;''', '''    bool enroll = false;
    bool verify = false;
    bool test = false;
    bool listCameras = false;
    bool probeCamera = false;
    int cameraIndex = 0;''', 'CLI flags')
setup = replace_once(setup, '''        else if (_wcsicmp(argv[i], L"--test") == 0)
        {
            test = true;
        }
        else if (_wcsicmp(argv[i], L"--camera") == 0 && i + 1 < argc)''', '''        else if (_wcsicmp(argv[i], L"--test") == 0)
        {
            test = true;
        }
        else if (_wcsicmp(argv[i], L"--list-cameras") == 0)
        {
            listCameras = true;
        }
        else if (_wcsicmp(argv[i], L"--probe-camera") == 0)
        {
            probeCamera = true;
        }
        else if (_wcsicmp(argv[i], L"--camera") == 0 && i + 1 < argc)''', 'CLI parser')
setup = replace_once(setup, '''    if (test)
    {
        return RunDiagnostics() ? 0 : 1;
    }

    if (enroll)''', '''    if (listCameras)
    {
        const auto devices = EnumerateCameraDevices();
        for (const auto& device : devices)
        {
            std::wcout << device.index << L"\t" << device.name << std::endl;
        }
        return devices.empty() ? 1 : 0;
    }

    if (probeCamera)
    {
        return ProbeCamera(cameraIndex) ? 0 : 1;
    }

    if (test)
    {
        return RunDiagnostics() ? 0 : 1;
    }

    if (enroll)''', 'CLI camera handlers')
setup = replace_once(setup, 'if (!SaveConfig(sid))', 'if (!SaveConfig(sid, cameraIndex))', 'selected camera persistence')
write(setup_path, setup)

vcx_path = 'src/setup/FaceLogonSetup.vcxproj'
vcx = read(vcx_path)
vcx = vcx.replace('Crypt32.lib;Shlwapi.lib;opencv_world4100d.lib;', 'Crypt32.lib;Shlwapi.lib;Strmiids.lib;Ole32.lib;OleAut32.lib;opencv_world4100d.lib;')
vcx = vcx.replace('Crypt32.lib;Shlwapi.lib;opencv_world4100.lib;', 'Crypt32.lib;Shlwapi.lib;Strmiids.lib;Ole32.lib;OleAut32.lib;opencv_world4100.lib;')
write(vcx_path, vcx)

lib_rs = r'''use serde::Serialize;
use std::io::{Read, Write};
use std::os::windows::process::CommandExt;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};
use winreg::enums::*;
use winreg::RegKey;

const CREATE_NO_WINDOW: u32 = 0x08000000;
const SETUP_FILES: [&str; 4] = ["secret.bin", "pin.bin", "config.json", "face.bin"];

#[derive(Serialize)]
pub struct RegistryStatus {
    pub enabled: bool,
    pub sid: String,
    pub threshold: f64,
    pub required_matches: i32,
    pub window_size: i32,
    pub scan_timeout_ms: i32,
    pub liveness_enabled: bool,
    pub camera_index: i32,
    pub setup_complete: bool,
}

#[derive(Serialize)]
pub struct CameraDevice {
    pub index: i32,
    pub name: String,
}

#[derive(Clone)]
struct FileSnapshot {
    path: PathBuf,
    contents: Option<Vec<u8>>,
}

fn get_enabled() -> bool {
    let hk = RegKey::predef(HKEY_LOCAL_MACHINE);
    hk.open_subkey("SOFTWARE\\HomeFaceLogon")
        .and_then(|key: RegKey| key.get_value::<u32, _>("Enabled"))
        .map(|value| value != 0)
        .unwrap_or(false)
}

fn set_enabled(enabled: bool) -> Result<(), String> {
    let hk = RegKey::predef(HKEY_LOCAL_MACHINE);
    let (key, _) = hk.create_subkey("SOFTWARE\\HomeFaceLogon").map_err(|e| e.to_string())?;
    key.set_value("Enabled", &(if enabled { 1u32 } else { 0u32 }))
        .map_err(|e| e.to_string())
}

fn data_dir() -> Result<PathBuf, String> {
    Ok(PathBuf::from(std::env::var("ProgramData")
        .map_err(|_| "ProgramData env var not found".to_string())?)
        .join("HomeFaceLogon"))
}

fn config_path() -> Result<PathBuf, String> {
    Ok(data_dir()?.join("config.json"))
}

fn find_setup_exe() -> Result<PathBuf, String> {
    let installed = PathBuf::from(r"C:\Program Files\HomeFaceLogon\FaceLogonSetup.exe");
    if installed.exists() { return Ok(installed); }
    for path in [
        r"..\x64\Debug\FaceLogonSetup.exe", r"..\x64\Release\FaceLogonSetup.exe",
        r"..\setup\x64\Debug\FaceLogonSetup.exe", r"..\setup\x64\Release\FaceLogonSetup.exe",
        r"..\..\x64\Debug\FaceLogonSetup.exe", r"..\..\x64\Release\FaceLogonSetup.exe",
        r"..\..\..\x64\Debug\FaceLogonSetup.exe", r"..\..\..\x64\Release\FaceLogonSetup.exe",
        r"..\..\..\..\x64\Debug\FaceLogonSetup.exe", r"..\..\..\..\x64\Release\FaceLogonSetup.exe",
    ] {
        let candidate = PathBuf::from(path);
        if candidate.exists() {
            if let Ok(abs) = std::fs::canonicalize(candidate) { return Ok(abs); }
        }
    }
    Err("FaceLogonSetup.exe not found. Build Release x64 first.".to_string())
}

fn run_setup_process(args: &[String], stdin_content: Option<&str>, timeout: Duration) -> Result<String, String> {
    let setup_exe = find_setup_exe()?;
    let mut child = Command::new(&setup_exe)
        .args(args)
        .stdin(if stdin_content.is_some() { Stdio::piped() } else { Stdio::null() })
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("Failed to start {}: {}", setup_exe.display(), e))?;

    let stdout_reader = child.stdout.take().map(|mut stream| thread::spawn(move || {
        let mut bytes = Vec::new(); let _ = stream.read_to_end(&mut bytes); bytes
    }));
    let stderr_reader = child.stderr.take().map(|mut stream| thread::spawn(move || {
        let mut bytes = Vec::new(); let _ = stream.read_to_end(&mut bytes); bytes
    }));

    if let Some(content) = stdin_content {
        let mut stdin = child.stdin.take().ok_or("Failed to open setup stdin")?;
        stdin.write_all(content.as_bytes()).map_err(|e| e.to_string())?;
    }

    let started = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if started.elapsed() < timeout => thread::sleep(Duration::from_millis(100)),
            Ok(None) => {
                let _ = child.kill(); let _ = child.wait();
                let stdout = stdout_reader.and_then(|r| r.join().ok()).unwrap_or_default();
                let stderr = stderr_reader.and_then(|r| r.join().ok()).unwrap_or_default();
                return Err(format!("Camera helper timed out after {} seconds and was terminated.\n{}{}",
                    timeout.as_secs(), String::from_utf8_lossy(&stdout), String::from_utf8_lossy(&stderr)));
            }
            Err(e) => { let _ = child.kill(); let _ = child.wait(); return Err(e.to_string()); }
        }
    };

    let stdout = stdout_reader.and_then(|r| r.join().ok()).unwrap_or_default();
    let stderr = stderr_reader.and_then(|r| r.join().ok()).unwrap_or_default();
    let out = String::from_utf8_lossy(&stdout).into_owned();
    let err = String::from_utf8_lossy(&stderr).into_owned();
    if status.success() { Ok(out) } else if err.trim().is_empty() { Err(out) } else { Err(err) }
}

fn snapshot_files() -> Result<Vec<FileSnapshot>, String> {
    let dir = data_dir()?;
    SETUP_FILES.iter().map(|name| {
        let path = dir.join(name);
        let contents = if path.exists() { Some(std::fs::read(&path).map_err(|e| e.to_string())?) } else { None };
        Ok(FileSnapshot { path, contents })
    }).collect()
}

fn restore_files(snapshot: &[FileSnapshot]) -> Result<(), String> {
    for item in snapshot {
        match &item.contents {
            Some(contents) => {
                if let Some(parent) = item.path.parent() { std::fs::create_dir_all(parent).map_err(|e| e.to_string())?; }
                std::fs::write(&item.path, contents).map_err(|e| e.to_string())?;
            }
            None => { if item.path.exists() { std::fs::remove_file(&item.path).map_err(|e| e.to_string())?; } }
        }
    }
    Ok(())
}

fn setup_files_complete() -> bool {
    let Ok(dir) = data_dir() else { return false; };
    SETUP_FILES.iter().all(|name| std::fs::metadata(dir.join(name))
        .map(|m| m.is_file() && m.len() > 0).unwrap_or(false))
}

#[tauri::command]
fn get_registry_status() -> Result<RegistryStatus, String> {
    let mut enabled = get_enabled();
    let mut sid = String::new();
    let mut threshold = 0.363;
    let mut required_matches = 3;
    let mut window_size = 5;
    let mut scan_timeout_ms = 10000;
    let mut liveness_enabled = false;
    let mut camera_index = 0;
    if let Ok(path) = config_path() {
        if let Ok(content) = std::fs::read_to_string(path) {
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(&content) {
                sid = v["targetSid"].as_str().unwrap_or_default().to_string();
                threshold = v["matchThreshold"].as_f64().unwrap_or(threshold);
                required_matches = v["requiredMatches"].as_i64().unwrap_or(3) as i32;
                window_size = v["windowSize"].as_i64().unwrap_or(5) as i32;
                scan_timeout_ms = v["scanTimeoutMs"].as_i64().unwrap_or(10000) as i32;
                liveness_enabled = v["livenessEnabled"].as_bool().unwrap_or(false);
                camera_index = v["cameraIndex"].as_i64().unwrap_or(0) as i32;
            }
        }
    }
    let setup_complete = !sid.is_empty() && setup_files_complete();
    if enabled && !setup_complete { let _ = set_enabled(false); enabled = false; }
    Ok(RegistryStatus { enabled, sid, threshold, required_matches, window_size,
        scan_timeout_ms, liveness_enabled, camera_index, setup_complete })
}

#[tauri::command]
fn set_registry_status(enabled: bool, sid: String, threshold: f64, required_matches: i32,
    window_size: i32, scan_timeout_ms: i32, liveness_enabled: bool, camera_index: i32) -> Result<(), String> {
    if enabled && (!setup_files_complete() || sid.trim().is_empty()) {
        return Err("セットアップが不完全です。資格情報・PIN・顔登録を完了してください。".to_string());
    }
    let path = config_path()?;
    if let Some(parent) = path.parent() { std::fs::create_dir_all(parent).map_err(|e| e.to_string())?; }
    let value = serde_json::json!({
        "schemaVersion": 1, "enabled": enabled, "targetSid": sid,
        "matchThreshold": threshold, "requiredMatches": required_matches,
        "windowSize": window_size, "scanTimeoutMs": scan_timeout_ms,
        "livenessEnabled": liveness_enabled, "cameraIndex": camera_index,
        "hostStartupTimeoutMs": 65000, "modelVersion": "1.0"
    });
    std::fs::write(path, serde_json::to_string_pretty(&value).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    set_enabled(enabled)
}

#[tauri::command]
fn get_user_sid() -> Result<String, String> {
    let output = Command::new("powershell").args(["-NoProfile", "-Command",
        "[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value"])
        .creation_flags(CREATE_NO_WINDOW).output().map_err(|e| e.to_string())?;
    let sid = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if !sid.is_empty() { return Ok(sid); }
    Err("Could not retrieve current user's SID.".to_string())
}

#[tauri::command]
fn list_cameras() -> Result<Vec<CameraDevice>, String> {
    let output = run_setup_process(&["--list-cameras".to_string()], None, Duration::from_secs(15))?;
    let devices: Vec<CameraDevice> = output.lines().filter_map(|line| {
        let (index, name) = line.split_once('\t')?;
        Some(CameraDevice { index: index.trim().parse().ok()?, name: name.trim().to_string() })
    }).collect();
    if devices.is_empty() { Err("利用可能なカメラが見つかりません。".to_string()) } else { Ok(devices) }
}

#[tauri::command]
fn run_complete_setup(sid: String, password: String, pin: String, camera_index: i32) -> Result<String, String> {
    if sid.trim().is_empty() || password.is_empty() || pin.is_empty() {
        return Err("SID、パスワード、PINは必須です。".to_string());
    }
    let snapshot = snapshot_files()?;
    let result = (|| {
        run_setup_process(&["--probe-camera".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(30))?;
        run_setup_process(&["--enroll".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(180))?;
        run_setup_process(&["--sid".into(), sid, "--camera".into(), camera_index.to_string()],
            Some(&format!("{}\n{}\n", password, pin)), Duration::from_secs(30))
    })();
    match result {
        Ok(output) => Ok(output),
        Err(error) => {
            let rollback = restore_files(&snapshot);
            let _ = set_enabled(false);
            match rollback {
                Ok(()) => Err(format!("{}\n変更は元の状態へ戻しました。", error)),
                Err(e) => Err(format!("{}\nロールバックにも失敗しました: {}", error, e)),
            }
        }
    }
}

#[tauri::command]
fn run_enroll(camera_index: i32) -> Result<String, String> {
    run_setup_process(&["--enroll".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(180))
}
#[tauri::command]
fn run_verify(camera_index: i32) -> Result<String, String> {
    run_setup_process(&["--verify".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(90))
}
#[tauri::command]
fn run_test() -> Result<String, String> {
    run_setup_process(&["--test".into()], None, Duration::from_secs(60))
}
#[tauri::command]
fn read_log_file(name: String) -> Result<String, String> {
    if name != "FaceLogonHost.log" && name != "FaceLogonSetup.log" && name != "FaceLogon.log" {
        return Err("Access denied: invalid log file name".to_string());
    }
    let path = data_dir()?.join("logs").join(name);
    if !path.exists() { return Ok("No log content found yet.".to_string()); }
    let content = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let lines: Vec<&str> = content.lines().collect();
    Ok(lines[lines.len().saturating_sub(100)..].join("\n"))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            if cfg!(debug_assertions) {
                app.handle().plugin(tauri_plugin_log::Builder::default()
                    .level(log::LevelFilter::Info).build())?;
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![get_registry_status, set_registry_status,
            get_user_sid, list_cameras, run_complete_setup, run_enroll, run_verify,
            run_test, read_log_file])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
'''
write('src/gui/src-tauri/src/lib.rs', lib_rs)

build_rs = r'''fn main() {
    let windows = tauri_build::WindowsAttributes::new().app_manifest(r#"
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency><dependentAssembly><assemblyIdentity type="win32"
    name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
    processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*" />
  </dependentAssembly></dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
    <requestedExecutionLevel level="requireAdministrator" uiAccess="false" />
  </requestedPrivileges></security></trustInfo>
</assembly>
"#);
    let attrs = tauri_build::Attributes::new().windows_attributes(windows);
    tauri_build::try_build(attrs).expect("failed to run tauri build script");
}
'''
write('src/gui/src-tauri/build.rs', build_rs)

app_path = 'src/gui/src/App.tsx'
app = read(app_path)
app = replace_once(app, '''interface RegistryStatus {
  enabled: boolean;''', '''interface CameraDevice {
  index: number;
  name: string;
}

interface RegistryStatus {
  enabled: boolean;''', 'CameraDevice interface')
app = replace_once(app, '''  camera_index: number;
}''', '''  camera_index: number;
  setup_complete: boolean;
}''', 'setup complete interface')
app = replace_once(app, '''    liveness_enabled: false,
    camera_index: 0,
  });''', '''    liveness_enabled: false,
    camera_index: 0,
    setup_complete: false,
  });''', 'setup complete default')
app = replace_once(app, '''  const [cameraIndex, setCameraIndex] = useState<number>(0);

  // App Config State''', '''  const [cameraIndex, setCameraIndex] = useState<number>(0);
  const [cameraDevices, setCameraDevices] = useState<CameraDevice[]>([]);
  const [cameraListError, setCameraListError] = useState<string>("");

  // App Config State''', 'camera state')
app = replace_once(app, '  const isConfigured = !!config.sid;', '  const isConfigured = config.setup_complete;', 'complete state')
app = replace_once(app, '''  useEffect(() => {
    loadConfig();
  }, []);''', '''  useEffect(() => {
    loadConfig();
    loadCameras();
  }, []);''', 'load cameras')
app = replace_once(app, '  const showStatus = (\n', '''  const loadCameras = async () => {
    try {
      const devices = await invoke<CameraDevice[]>("list_cameras");
      setCameraDevices(devices);
      setCameraListError("");
      if (!devices.some((device) => device.index === cameraIndex)) {
        setCameraIndex(devices[0].index);
      }
    } catch (err: any) {
      setCameraDevices([]);
      setCameraListError(String(err));
    }
  };

  const showStatus = (
''', 'loadCameras function')

start = app.find('  const handleSaveSetupCredentials = async () => {')
end = app.find('  const handleStartEnrollment = async () => {', start)
if start < 0 or end < 0: raise RuntimeError('setup handler markers missing')
app = app[:start] + r'''  const handleSaveSetupCredentials = async () => {
    if (!sid) {
      showStatus("SIDを入力してください。", "error");
      return;
    }
    if (!password) {
      showStatus("Windowsサインインパスワードを入力してください。", "error");
      return;
    }
    if (pin.length < 4 || pin.length > 16 || !/^[\x21-\x7E]+$/.test(pin)) {
      showStatus("PINは4〜16文字の半角英数字・記号（スペース除く）で入力してください。", "error");
      return;
    }
    // Do not write partial credentials yet. Commit the entire setup only after
    // camera probing and face enrollment succeed.
    setWizardStep(3);
    await loadCameras();
  };

''' + app[end:]

start = app.find('  const handleStartEnrollment = async () => {')
end = app.find('  const handleSaveSettings = async () => {', start)
if start < 0 or end < 0: raise RuntimeError('enrollment handler markers missing')
app = app[:start] + r'''  const handleStartEnrollment = async () => {
    if (!cameraDevices.some((device) => device.index === cameraIndex)) {
      showStatus("一覧から有効なカメラを選択してください。", "error");
      return;
    }
    setLoading(true);
    try {
      showStatus("カメラ確認後に顔登録を開始します。失敗時は設定を元に戻します。", "info");
      await invoke<string>("run_complete_setup", { sid, password, pin, cameraIndex });
      setPassword("");
      setPin("");
      showStatus("資格情報・PIN・顔情報を一括で登録しました。", "success");
      setWizardStep(4);
      await loadConfig();
    } catch (err: any) {
      showStatus(`セットアップに失敗しました: ${err}`, "error");
    } finally {
      setLoading(false);
    }
  };

''' + app[end:]

app = app.replace('<div>v0.2.0</div>', '<div>v0.3.4 camera recovery</div>')
old = '''                  <select
                    className="form-input"
                    value={cameraIndex}
                    onChange={(e) => setCameraIndex(parseInt(e.target.value))}
                    style={{ maxWidth: "200px" }}
                  >
                    <option value={0}>カメラ 0 (デフォルト)</option>
                    <option value={1}>カメラ 1</option>
                    <option value={2}>カメラ 2</option>
                  </select>'''
new = '''                  <div className="form-input-container">
                    <select
                      className="form-input"
                      value={cameraIndex}
                      onChange={(e) => setCameraIndex(parseInt(e.target.value))}
                      style={{ maxWidth: "420px" }}
                      disabled={cameraDevices.length === 0}
                    >
                      {cameraDevices.length === 0 ? (
                        <option value={-1}>カメラが見つかりません</option>
                      ) : (
                        cameraDevices.map((device) => (
                          <option key={device.index} value={device.index}>
                            [{device.index}] {device.name}
                          </option>
                        ))
                      )}
                    </select>
                    <button className="btn btn-secondary" onClick={loadCameras} disabled={loading}>
                      再読み込み
                    </button>
                  </div>
                  {cameraListError && (
                    <span className="form-help" style={{ color: "var(--danger)" }}>
                      {cameraListError}
                    </span>
                  )}'''
app = replace_once(app, old, new, 'wizard camera select')
app = replace_once(app, '''                    disabled={loading}
                  >
                    {loading
                      ? "カメラ起動中..."''', '''                    disabled={loading || cameraDevices.length === 0}
                  >
                    {loading
                      ? "カメラ起動中..."''', 'disable enrollment')
old = '''                <select
                  className="form-input"
                  value={config.camera_index}
                  onChange={(e) =>
                    setConfig((prev) => ({
                      ...prev,
                      camera_index: parseInt(e.target.value),
                    }))
                  }
                  style={{ maxWidth: "240px" }}
                >
                  <option value={0}>0 (デフォルト / 内蔵等)</option>
                  <option value={1}>1</option>
                  <option value={2}>2</option>
                  <option value={3}>3</option>
                </select>
                <span className="form-help">
                  複数のカメラが接続されている場合、使用するカメラのインデックスを選択します。
                </span>'''
new = '''                <div className="form-input-container">
                  <select
                    className="form-input"
                    value={config.camera_index}
                    onChange={(e) => setConfig((prev) => ({ ...prev, camera_index: parseInt(e.target.value) }))}
                    style={{ maxWidth: "420px" }}
                    disabled={cameraDevices.length === 0}
                  >
                    {cameraDevices.length === 0 ? (
                      <option value={-1}>カメラが見つかりません</option>
                    ) : (
                      cameraDevices.map((device) => (
                        <option key={device.index} value={device.index}>
                          [{device.index}] {device.name}
                        </option>
                      ))
                    )}
                  </select>
                  <button className="btn btn-secondary" onClick={loadCameras} disabled={loading}>
                    再読み込み
                  </button>
                </div>
                <span className="form-help">Windowsが報告した具体的なデバイス名から選択します。</span>'''
app = replace_once(app, old, new, 'settings camera select')
write(app_path, app)

iss_path = 'installer/HomeFaceLogon.iss'
iss = read(iss_path).replace('AppVersion=0.3.3', 'AppVersion=0.3.4')
iss = iss.replace('OutputBaseFilename=HomeFaceLogonSetup-v0.3.3', 'OutputBaseFilename=HomeFaceLogonSetup-v0.3.4-camera-recovery')
iss = iss.replace('Flags: postinstall nowait runascurrentuser', 'Flags: postinstall nowait')
write(iss_path, iss)
write('src/gui/src-tauri/tauri.conf.json', read('src/gui/src-tauri/tauri.conf.json').replace('"version": "0.1.0"', '"version": "0.3.4"'))
write('src/gui/src-tauri/Cargo.toml', read('src/gui/src-tauri/Cargo.toml').replace('version = "0.1.0"', 'version = "0.3.4"', 1))
print('Applied camera recovery and transactional setup patch.')

