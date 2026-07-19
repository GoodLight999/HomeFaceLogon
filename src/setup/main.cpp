#include <windows.h>
#include <dshow.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <filesystem>
#include <memory>
#include <cmath>
#include <thread>
#include <atomic>

// OpenCV headers
#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/dnn.hpp>

#include <bcrypt.h>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Strmiids.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

namespace fs = std::filesystem;

struct SecretFileHeader {
    uint32_t magic;          // 'HFLO' (0x4F4C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t protectedBlobSize;
};

struct FaceFileHeader {
    uint32_t magic;          // 'HFLF' (0x464C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t protectedBlobSize;
};

const uint32_t HFLO_MAGIC = 0x4F4C4648;
const uint32_t HFLF_MAGIC = 0x464C4648;

static constexpr int kCameraWidth = 640;
static constexpr int kCameraHeight = 480;

// MSMF/DirectShow camera backends require COM on the thread that opens and
// uses VideoCapture. Worker-thread camera init races with DNN model loading
// and can deadlock during MFStartup/COM apartment setup.
struct ComApartment
{
    HRESULT hr;

    ComApartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment()
    {
        if (SUCCEEDED(hr))
        {
            CoUninitialize();
        }
    }

    bool Ok() const
    {
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
};

struct CameraDeviceInfo
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

static void ConfigureCameraProperties(cv::VideoCapture& cap)
{
    cap.set(cv::CAP_PROP_FRAME_WIDTH, kCameraWidth);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, kCameraHeight);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
}

static bool OpenCameraBackend(cv::VideoCapture& cap, int cameraIndex, bool allowFallback)
{
    std::cout << "Attempting to open camera index " << cameraIndex
              << " using CAP_DSHOW (allowFallback="
              << (allowFallback ? "true" : "false") << ")..." << std::endl;
    cap.open(cameraIndex, cv::CAP_DSHOW);
    if (!cap.isOpened() && allowFallback)
    {
        std::cout << "DirectShow failed, trying Media Foundation (CAP_MSMF)..." << std::endl;
        cap.open(cameraIndex, cv::CAP_MSMF);
    }
    if (!cap.isOpened() && allowFallback)
    {
        std::cout << "Media Foundation failed, trying default backend..." << std::endl;
        cap.open(cameraIndex);
    }
    return cap.isOpened();
}

static bool WarmUpCamera(cv::VideoCapture& cap, int maxAttempts = 30, int delayMs = 100)
{
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        if (cap.grab())
        {
            return true;
        }
        Sleep(delayMs);
    }
    return false;
}

// USB cameras that were idle or in a power-saving state need time to resume before
// DirectShow/MSMF can enumerate and stream. Retry open/warm-up instead of failing
// the first verification or enrollment attempt.
static bool OpenAndConfigureCamera(cv::VideoCapture& cap, int cameraIndex, int& width, int& height)
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

    const int MAX_RETRIES = 3;
    const int RETRY_DELAY_MS = 500;

    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt)
    {
        const bool allowFallback = (attempt >= MAX_RETRIES - 1);
        std::cout << "Attempting to open camera index " << cameraIndex
                  << " (attempt " << attempt << "/" << MAX_RETRIES
                  << ", allowFallback=" << (allowFallback ? "true" : "false") << ")..." << std::endl;

        if (OpenCameraBackend(cap, cameraIndex, allowFallback))
        {
            ConfigureCameraProperties(cap);

            width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
            height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
            if (width <= 0 || height <= 0)
            {
                width = kCameraWidth;
                height = kCameraHeight;
            }

            // On early attempts use fewer warm-up polls so we fail fast and
            // retry the full open cycle rather than blocking on a camera that
            // is still re-enumerating after sleep/resume.
            int warmAttempts = (attempt < MAX_RETRIES) ? 10 : 30;
            if (WarmUpCamera(cap, warmAttempts))
            {
                std::cout << "Camera opened and streaming successfully ("
                          << width << "x" << height << ")." << std::endl;
                return true;
            }

            std::cout << "Camera opened but failed to stream frames after warm-up. Releasing for retry..." << std::endl;
            cap.release();
        }

        if (attempt < MAX_RETRIES)
        {
            std::cout << "Camera device busy or resuming from power saving. Waiting "
                      << RETRY_DELAY_MS << " ms before retry..." << std::endl;
            Sleep(RETRY_DELAY_MS);
        }
    }

    return false;
}

struct HostConfig {
    double matchThreshold = 0.363;
};

static double ExtractDouble(const std::string& content, const std::string& key, double defaultValue)
{
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return defaultValue;
    pos = content.find(":", pos);
    if (pos == std::string::npos) return defaultValue;
    size_t valPos = content.find_first_not_of(" \t\r\n", pos + 1);
    if (valPos == std::string::npos) return defaultValue;
    try {
        return std::stod(content.substr(valPos));
    } catch(...) {
        return defaultValue;
    }
}

static bool LoadHostConfig(HostConfig& config)
{
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    PathAppendW(path, L"config.json");

    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    config.matchThreshold = ExtractDouble(content, "matchThreshold", 0.363);
    if (config.matchThreshold <= 0.0 || config.matchThreshold > 1.0) config.matchThreshold = 0.363;
    return true;
}

std::string utf16_to_utf8(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring GetModelPath(const std::wstring& filename) {
    wchar_t programData[MAX_PATH];
    
    // 1. Check %ProgramData%\HomeFaceLogon\models\<filename>
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        fs::path p1 = fs::path(programData) / L"HomeFaceLogon" / L"models" / filename;
        if (fs::exists(p1)) return p1.wstring();
        
        fs::path p2 = fs::path(programData) / L"HomeFaceLogon" / filename;
        if (fs::exists(p2)) return p2.wstring();
    }

    // 2. Check relative to executable path L"models/" + filename
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        fs::path exeDir = fs::path(exePath).parent_path();
        fs::path p3 = exeDir / L"models" / filename;
        if (fs::exists(p3)) return p3.wstring();
        
        fs::path p4 = exeDir / filename;
        if (fs::exists(p4)) return p4.wstring();
    }

    // 3. Check relative to current working directory
    fs::path p5 = fs::path(L"models") / filename;
    if (fs::exists(p5)) return p5.wstring();

    fs::path p6 = filename;
    if (fs::exists(p6)) return p6.wstring();

    // Fallback path in %ProgramData%
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        return (fs::path(programData) / L"HomeFaceLogon" / L"models" / filename).wstring();
    }
    return filename;
}

template <typename Writer>
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

bool SaveSecret(const std::wstring& password)
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

struct PinFileHeader {
    uint32_t magic;          // 'HFLP' (0x504C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t iterations;     // PBKDF2 iterations
    uint8_t  salt[16];       // Random salt
};

const uint32_t HFLP_MAGIC = 0x504C4648;
const uint32_t PIN_HASH_SIZE = 32;

bool SavePin(const std::wstring& pin)
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

bool SaveConfig(const std::wstring& sid, int cameraIndex)
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

bool SaveFaceTemplate(const std::vector<float>& featureVec)
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

bool LoadFaceTemplate(std::vector<float>& featureVec)
{
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    PathAppendW(path, L"face.bin");

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open face.bin for reading. Make sure to --enroll first." << std::endl;
        return false;
    }

    FaceFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != HFLF_MAGIC || header.schemaVersion != 1)
    {
        std::wcerr << L"Invalid face.bin header." << std::endl;
        return false;
    }

    if (header.protectedBlobSize > 65536)
    {
        std::wcerr << L"Invalid face.bin protected blob size." << std::endl;
        file.close();
        return false;
    }

    std::vector<BYTE> encryptedBlob(header.protectedBlobSize);
    file.read(reinterpret_cast<char*>(encryptedBlob.data()), header.protectedBlobSize);
    file.close();

    DATA_BLOB input;
    input.pbData = encryptedBlob.data();
    input.cbData = header.protectedBlobSize;

    DATA_BLOB output = {0};

    if (!CryptUnprotectData(
        &input,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        CRYPTPROTECT_UI_FORBIDDEN,
        &output
    ))
    {
        std::wcerr << L"CryptUnprotectData failed: " << GetLastError() << std::endl;
        return false;
    }

    if (output.cbData != 128 * sizeof(float))
    {
        std::wcerr << L"Decrypted face template size mismatch: " << output.cbData << L" bytes (expected 512)" << std::endl;
        LocalFree(output.pbData);
        return false;
    }

    featureVec.resize(128);
    memcpy(featureVec.data(), output.pbData, 128 * sizeof(float));
    LocalFree(output.pbData);
    return true;
}

bool EnrollFace(int cameraIndex)
{
    std::wstring wModelDet = GetModelPath(L"face_detection_yunet_2023mar.onnx");
    std::wstring wModelRec = GetModelPath(L"face_recognition_sface_2021dec.onnx");

    if (!fs::exists(wModelDet) || !fs::exists(wModelRec))
    {
        std::wcerr << L"Error: Required ONNX models are missing. Make sure to run tools/fetch-models.ps1." << std::endl;
        std::wcerr << L"Expected paths:\n  " << wModelDet << L"\n  " << wModelRec << std::endl;
        return false;
    }

    std::string modelDet = utf16_to_utf8(wModelDet);
    std::string modelRec = utf16_to_utf8(wModelRec);

    ComApartment com;
    if (!com.Ok())
    {
        std::wcerr << L"Error: Failed to initialize COM for camera access." << std::endl;
        return false;
    }

    cv::VideoCapture cap;
    cv::Ptr<cv::FaceDetectorYN> detector;
    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    int width = kCameraWidth;
    int height = kCameraHeight;
    std::atomic<bool> modelsOk{false};

    // Synchronous initialization to avoid COM and loader lock deadlocks.
    try
    {
        detector = cv::FaceDetectorYN::create(
            modelDet, "", cv::Size(kCameraWidth, kCameraHeight), 0.9f, 0.3f, 5000);
        recognizer = cv::FaceRecognizerSF::create(modelRec, "");
        modelsOk = true;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "Error creating face models: " << e.what() << std::endl;
    }

    const bool cameraOk = OpenAndConfigureCamera(cap, cameraIndex, width, height);

    if (!modelsOk)
    {
        return false;
    }

    if (!cameraOk)
    {
        std::wcerr << L"Error: Failed to open camera index " << cameraIndex << std::endl;
        return false;
    }

    std::cout << "Starting Face Enrollment. Please look at the camera." << std::endl;
    std::cout << "Collecting 30 face samples. Press ESC to cancel." << std::endl;

    std::vector<cv::Mat> collectedFeatures;
    cv::Mat frame;
    const int REQUIRED_SAMPLES = 30;

    while (collectedFeatures.size() < REQUIRED_SAMPLES)
    {
        cap >> frame;
        if (frame.empty())
        {
            std::wcerr << L"Failed to grab frame." << std::endl;
            break;
        }

        cv::Mat faces;
        detector->setInputSize(frame.size());
        detector->detect(frame, faces);

        std::string msg = "Enrolling: " + std::to_string(collectedFeatures.size()) + "/" + std::to_string(REQUIRED_SAMPLES);
        cv::Scalar boxColor = cv::Scalar(0, 0, 255); // Red by default

        if (faces.rows == 1)
        {
            cv::Mat aligned;
            recognizer->alignCrop(frame, faces.row(0), aligned);
            cv::Mat feature;
            recognizer->feature(aligned, feature);
            collectedFeatures.push_back(feature.clone());
            
            boxColor = cv::Scalar(0, 255, 0); // Green when exactly one face detected
        }
        else if (faces.rows > 1)
        {
            msg = "Keep only ONE face in frame";
        }
        else
        {
            msg = "No face detected";
        }

        // Draw bounding boxes
        for (int i = 0; i < faces.rows; ++i)
        {
            int x = static_cast<int>(faces.at<float>(i, 0));
            int y = static_cast<int>(faces.at<float>(i, 1));
            int w = static_cast<int>(faces.at<float>(i, 2));
            int h = static_cast<int>(faces.at<float>(i, 3));
            cv::rectangle(frame, cv::Rect(x, y, w, h), boxColor, 2);
        }

        cv::putText(frame, msg, cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 0.8, boxColor, 2);
        cv::imshow("Face Enrollment - HomeFaceLogon", frame);

        // Check if window was closed by user
        if (cv::getWindowProperty("Face Enrollment - HomeFaceLogon", cv::WND_PROP_VISIBLE) < 1)
        {
            std::cout << "Enrollment cancelled by closing window." << std::endl;
            cv::destroyAllWindows();
            return false;
        }

        if (cv::waitKey(1) == 27)
        {
            std::cout << "Enrollment cancelled by user." << std::endl;
            cv::destroyAllWindows();
            return false;
        }
    }

    cv::destroyAllWindows();

    if (collectedFeatures.size() < REQUIRED_SAMPLES)
    {
        std::wcerr << L"Failed to collect enough samples." << std::endl;
        return false;
    }

    // Average features
    cv::Mat sumFeature = cv::Mat::zeros(1, 128, CV_32F);
    for (const auto& feat : collectedFeatures)
    {
        sumFeature += feat;
    }
    
    cv::Mat avgFeature;
    cv::normalize(sumFeature, avgFeature, 1, 0, cv::NORM_L2);

    std::vector<float> finalTemplate(128);
    if (avgFeature.isContinuous())
    {
        memcpy(finalTemplate.data(), avgFeature.ptr<float>(), 128 * sizeof(float));
    }
    else
    {
        for (int i = 0; i < 128; ++i)
        {
            finalTemplate[i] = avgFeature.at<float>(0, i);
        }
    }

    if (SaveFaceTemplate(finalTemplate))
    {
        std::cout << "Face template registered and saved successfully." << std::endl;
        return true;
    }
    else
    {
        std::wcerr << L"Failed to save face template." << std::endl;
        return false;
    }
}

bool VerifyFace(int cameraIndex)
{
    std::vector<float> templateVec;
    if (!LoadFaceTemplate(templateVec))
    {
        return false;
    }

    cv::Mat templateFeature = cv::Mat(1, 128, CV_32F, templateVec.data()).clone();

    std::wstring wModelDet = GetModelPath(L"face_detection_yunet_2023mar.onnx");
    std::wstring wModelRec = GetModelPath(L"face_recognition_sface_2021dec.onnx");

    if (!fs::exists(wModelDet) || !fs::exists(wModelRec))
    {
        std::wcerr << L"Error: Required ONNX models are missing. Make sure to run tools/fetch-models.ps1." << std::endl;
        return false;
    }

    std::string modelDet = utf16_to_utf8(wModelDet);
    std::string modelRec = utf16_to_utf8(wModelRec);

    ComApartment com;
    if (!com.Ok())
    {
        std::wcerr << L"Error: Failed to initialize COM for camera access." << std::endl;
        return false;
    }

    cv::VideoCapture cap;
    cv::Ptr<cv::FaceDetectorYN> detector;
    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    int width = kCameraWidth;
    int height = kCameraHeight;
    std::atomic<bool> modelsOk{false};

    // Synchronous initialization to avoid COM and loader lock deadlocks.
    try
    {
        detector = cv::FaceDetectorYN::create(
            modelDet, "", cv::Size(kCameraWidth, kCameraHeight), 0.9f, 0.3f, 5000);
        recognizer = cv::FaceRecognizerSF::create(modelRec, "");
        modelsOk = true;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "Error creating face models: " << e.what() << std::endl;
    }

    const bool cameraOk = OpenAndConfigureCamera(cap, cameraIndex, width, height);

    if (!modelsOk)
    {
        return false;
    }

    if (!cameraOk)
    {
        std::wcerr << L"Error: Failed to open camera index " << cameraIndex << std::endl;
        return false;
    }

    HostConfig config;
    LoadHostConfig(config);
    std::cout << "Starting Face Verification (threshold=" << config.matchThreshold << "). Press ESC to stop." << std::endl;

    cv::Mat frame;
    int matchCount = 0;
    bool verifySuccess = false;

    while (true)
    {
        cap >> frame;
        if (frame.empty())
        {
            std::wcerr << L"Failed to grab frame." << std::endl;
            break;
        }

        cv::Mat faces;
        detector->setInputSize(frame.size());
        detector->detect(frame, faces);

        if (faces.rows > 0)
        {
            for (int i = 0; i < faces.rows; ++i)
            {
                cv::Mat aligned;
                recognizer->alignCrop(frame, faces.row(i), aligned);
                cv::Mat feature;
                recognizer->feature(aligned, feature);

                double score = recognizer->match(feature, templateFeature, cv::FaceRecognizerSF::DisType::FR_COSINE);

                bool isMatch = score >= config.matchThreshold;
                if (isMatch)
                {
                    matchCount++;
                }

                cv::Scalar boxColor = isMatch ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);

                int x = static_cast<int>(faces.at<float>(i, 0));
                int y = static_cast<int>(faces.at<float>(i, 1));
                int w = static_cast<int>(faces.at<float>(i, 2));
                int h = static_cast<int>(faces.at<float>(i, 3));
                cv::rectangle(frame, cv::Rect(x, y, w, h), boxColor, 2);

                std::string label = "Score: " + std::to_string(score).substr(0, 5) + (isMatch ? " (OK)" : " (NG)");
                cv::putText(frame, label, cv::Point(x, y - 10), cv::FONT_HERSHEY_SIMPLEX, 0.6, boxColor, 2);
            }
        }

        cv::imshow("Face Verification - HomeFaceLogon", frame);

        // Auto close on 3 matches
        if (matchCount >= 3)
        {
            std::cout << "Verification successful. Match count threshold reached." << std::endl;
            verifySuccess = true;
            cv::waitKey(1000); // Show result for 1 second
            break;
        }

        // Check if window was closed
        if (cv::getWindowProperty("Face Verification - HomeFaceLogon", cv::WND_PROP_VISIBLE) < 1)
        {
            std::cout << "Verification stopped by closing window." << std::endl;
            break;
        }

        if (cv::waitKey(1) == 27)
        {
            std::cout << "Verification cancelled by user." << std::endl;
            break;
        }
    }

    cv::destroyAllWindows();
    return verifySuccess;
}

bool ProbeCamera(int cameraIndex)
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

bool RunDiagnostics()
{
    std::cout << "[Diagnostic] Starting system diagnostic checks..." << std::endl;

    // 1. Model file checks
    std::wstring wModelDet = GetModelPath(L"face_detection_yunet_2023mar.onnx");
    std::wstring wModelRec = GetModelPath(L"face_recognition_sface_2021dec.onnx");

    std::wcout << L"[Diagnostic] Detector model path: " << wModelDet << std::endl;
    std::wcout << L"[Diagnostic] Recognizer model path: " << wModelRec << std::endl;

    if (!fs::exists(wModelDet))
    {
        std::wcerr << L"[Diagnostic] ERROR: Detector model file not found." << std::endl;
        return false;
    }
    if (!fs::exists(wModelRec))
    {
        std::wcerr << L"[Diagnostic] ERROR: Recognizer model file not found." << std::endl;
        return false;
    }
    std::cout << "[Diagnostic] OK: Model files exist." << std::endl;

    // 2. Initialize OpenCV models
    std::string modelDet = utf16_to_utf8(wModelDet);
    std::string modelRec = utf16_to_utf8(wModelRec);

    try
    {
        cv::Ptr<cv::FaceDetectorYN> detector = cv::FaceDetectorYN::create(modelDet, "", cv::Size(640, 480), 0.9f, 0.3f, 5000);
        std::cout << "[Diagnostic] OK: FaceDetectorYN initialized successfully." << std::endl;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[Diagnostic] ERROR: Failed to initialize FaceDetectorYN: " << e.what() << std::endl;
        return false;
    }

    try
    {
        cv::Ptr<cv::FaceRecognizerSF> recognizer = cv::FaceRecognizerSF::create(modelRec, "");
        std::cout << "[Diagnostic] OK: FaceRecognizerSF initialized successfully." << std::endl;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "[Diagnostic] ERROR: Failed to initialize FaceRecognizerSF: " << e.what() << std::endl;
        return false;
    }

    // 3. DPAPI encryption/decryption check
    std::cout << "[Diagnostic] Testing DPAPI face template storage..." << std::endl;
    std::vector<float> dummyTemplate(128);
    for (int i = 0; i < 128; ++i)
    {
        dummyTemplate[i] = static_cast<float>(i) / 128.0f;
    }

    // Backup existing face.bin if it exists
    wchar_t path[MAX_PATH];
    bool hasBackup = false;
    std::wstring backupPath;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        fs::path facePath = fs::path(path) / L"HomeFaceLogon" / L"face.bin";
        // Ensure parent directory exists for dummy test
        fs::create_directories(facePath.parent_path());
        if (fs::exists(facePath))
        {
            backupPath = facePath.wstring() + L".bak";
            try {
                fs::copy_file(facePath, backupPath, fs::copy_options::overwrite_existing);
                hasBackup = true;
            } catch(...) {}
        }
    }

    if (!SaveFaceTemplate(dummyTemplate))
    {
        std::cerr << "[Diagnostic] ERROR: SaveFaceTemplate failed." << std::endl;
        return false;
    }
    std::cout << "[Diagnostic] OK: Dummy face template encrypted and saved." << std::endl;

    std::vector<float> loadedTemplate;
    if (!LoadFaceTemplate(loadedTemplate))
    {
        std::cerr << "[Diagnostic] ERROR: LoadFaceTemplate failed." << std::endl;
        return false;
    }
    std::cout << "[Diagnostic] OK: Dummy face template loaded and decrypted." << std::endl;

    if (loadedTemplate.size() != 128)
    {
        std::cerr << "[Diagnostic] ERROR: Decrypted template size is incorrect: " << loadedTemplate.size() << std::endl;
        return false;
    }

    bool match = true;
    for (int i = 0; i < 128; ++i)
    {
        if (std::abs(loadedTemplate[i] - dummyTemplate[i]) > 1e-5)
        {
            match = false;
            break;
        }
    }

    if (!match)
    {
        std::cerr << "[Diagnostic] ERROR: Decrypted template values do not match original." << std::endl;
        return false;
    }
    std::cout << "[Diagnostic] OK: Decrypted template values match original dummy values." << std::endl;

    // Restore backup if existed, otherwise delete dummy face.bin
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        fs::path facePath = fs::path(path) / L"HomeFaceLogon" / L"face.bin";
        if (hasBackup)
        {
            try {
                fs::copy_file(backupPath, facePath, fs::copy_options::overwrite_existing);
                fs::remove(backupPath);
                std::cout << "[Diagnostic] Restored original face.bin." << std::endl;
            } catch(...) {}
        }
        else
        {
            try {
                fs::remove(facePath);
                std::cout << "[Diagnostic] Cleaned up temporary face.bin." << std::endl;
            } catch(...) {}
        }
    }

    std::cout << "[Diagnostic] Diagnostic test PASSED." << std::endl;
    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    std::wstring sid;
    std::wstring password;
    std::wstring pin;
    bool enroll = false;
    bool verify = false;
    bool test = false;
    bool listCameras = false;
    bool probeCamera = false;
    int cameraIndex = 0;

    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--sid") == 0 && i + 1 < argc)
        {
            sid = argv[++i];
        }
        else if (_wcsicmp(argv[i], L"--enroll") == 0)
        {
            enroll = true;
        }
        else if (_wcsicmp(argv[i], L"--verify") == 0)
        {
            verify = true;
        }
        else if (_wcsicmp(argv[i], L"--test") == 0)
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
        else if (_wcsicmp(argv[i], L"--camera") == 0 && i + 1 < argc)
        {
            cameraIndex = _wtoi(argv[++i]);
        }
    }

    if (listCameras)
    {
        const auto devices = EnumerateCameraDevices();
        for (const auto& device : devices)
        {
            std::wcout << device.index << L"	" << device.name << std::endl;
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

    if (enroll)
    {
        return EnrollFace(cameraIndex) ? 0 : 1;
    }

    if (verify)
    {
        return VerifyFace(cameraIndex) ? 0 : 1;
    }

    if (sid.empty())
    {
        std::wcout << L"Enter Target Windows User SID: ";
        std::getline(std::wcin, sid);
        if (!sid.empty() && sid.back() == L'\r')
        {
            sid.pop_back();
        }
    }

    if (password.empty())
    {
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        BOOL isConsole = GetConsoleMode(hStdin, &mode);
        if (isConsole)
        {
            std::wcout << L"Enter Microsoft Account Password: ";
            SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
        }
        
        std::getline(std::wcin, password);
        if (!password.empty() && password.back() == L'\r')
        {
            password.pop_back();
        }
        
        if (isConsole)
        {
            SetConsoleMode(hStdin, mode);
            std::wcout << std::endl;
        }
    }

    if (pin.empty())
    {
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        BOOL isConsole = GetConsoleMode(hStdin, &mode);
        if (isConsole)
        {
            std::wcout << L"Enter Local PIN (4-16 digits for fallback): ";
            SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
        }
        
        std::getline(std::wcin, pin);
        if (!pin.empty() && pin.back() == L'\r')
        {
            pin.pop_back();
        }
        
        if (isConsole)
        {
            SetConsoleMode(hStdin, mode);
            std::wcout << std::endl;
        }
    }

    if (sid.empty() || password.empty() || pin.empty())
    {
        std::wcerr << L"Error: SID, Password, and PIN cannot be empty." << std::endl;
        return 1;
    }

    // PIN complexity check: 4 to 16 characters (ASCII printable, no spaces)
    if (pin.length() < 4 || pin.length() > 16)
    {
        std::wcerr << L"Error: PIN must be between 4 and 16 characters." << std::endl;
        SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
        SecureZeroMemory(&pin[0], pin.length() * sizeof(wchar_t));
        return 1;
    }
    for (wchar_t c : pin)
    {
        if (c < 33 || c > 126)
        {
            std::wcerr << L"Error: PIN must contain only alphanumeric characters and symbols (no spaces)." << std::endl;
            SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
            SecureZeroMemory(&pin[0], pin.length() * sizeof(wchar_t));
            return 1;
        }
    }


    if (!SaveSecret(password))
    {
        std::wcerr << L"Failed to save encrypted secret." << std::endl;
        SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
        SecureZeroMemory(&pin[0], pin.length() * sizeof(wchar_t));
        return 1;
    }

    if (!SavePin(pin))
    {
        std::wcerr << L"Failed to save PIN." << std::endl;
        SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
        SecureZeroMemory(&pin[0], pin.length() * sizeof(wchar_t));
        return 1;
    }

    if (!SaveConfig(sid, cameraIndex))
    {
        std::wcerr << L"Failed to save configuration." << std::endl;
        SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
        SecureZeroMemory(&pin[0], pin.length() * sizeof(wchar_t));
        return 1;
    }

    SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
    SecureZeroMemory(&pin[0], pin.length() * sizeof(wchar_t));

    std::wcout << L"Setup completed successfully. targetSid, secret.bin, and pin.bin updated." << std::endl;
    return 0;
}
