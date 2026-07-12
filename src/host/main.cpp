#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <ctime>
#include <cstdarg>
#include <opencv2/opencv.hpp>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "Shell32.lib")

struct MessageHeader {
    uint32_t magic;          // 'HFLO'
    uint16_t version;        // 1
    uint16_t type;
    uint32_t payloadSize;
    uint64_t sessionNonceHi;
    uint64_t sessionNonceLo;
};

const uint32_t MSG_MAGIC = 0x4F4C4648;
const uint16_t MSG_VERSION = 1;

enum MessageType : uint16_t {
    MSG_START_SCAN = 1,
    MSG_CANCEL_SCAN = 2,
    MSG_STATUS = 3,
    MSG_MATCHED = 4,
    MSG_NO_MATCH = 5,
    MSG_CAMERA_ERROR = 6,
    MSG_MODEL_ERROR = 7,
    MSG_SHUTDOWN = 8
};

struct FaceFileHeader {
    uint32_t magic;          // 'HFLF' (0x464C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t protectedBlobSize;
};

const uint32_t HFLF_MAGIC = 0x464C4648;

// Global state
std::wstring g_pipeName;
uint64_t g_nonceHi = 0;
uint64_t g_nonceLo = 0;
HANDLE g_hPipe = INVALID_HANDLE_VALUE;
std::atomic<bool> g_running{true};

void HostLog(const char* level, const char* format, ...)
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        PathAppendW(path, L"HomeFaceLogon");
        CreateDirectoryW(path, nullptr);
        PathAppendW(path, L"logs");
        CreateDirectoryW(path, nullptr);
        PathAppendW(path, L"FaceLogonHost.log");

        FILE* f = nullptr;
        _wfopen_s(&f, path, L"a");
        if (f)
        {
            time_t now = time(nullptr);
            tm timeinfo;
            localtime_s(&timeinfo, &now);
            char timeStr[64];
            strftime(timeStr, 64, "%Y-%m-%d %H:%M:%S", &timeinfo);

            fprintf(f, "[%s] [%s] ", timeStr, level);
            va_list args;
            va_start(args, format);
            vfprintf(f, format, args);
            va_end(args);
            fprintf(f, "\n");
            fclose(f);
        }
    }
}

bool SendPipeMessage(MessageType type)
{
    if (g_hPipe == INVALID_HANDLE_VALUE) return false;

    HostLog("INFO", "SendPipeMessage: type=%d start", type);

    MessageHeader header = {};
    header.magic = MSG_MAGIC;
    header.version = MSG_VERSION;
    header.type = type;
    header.payloadSize = 0;
    header.sessionNonceHi = g_nonceHi;
    header.sessionNonceLo = g_nonceLo;

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
    {
        HostLog("ERROR", "CreateEventW failed in SendPipeMessage.");
        return false;
    }

    DWORD bytesWritten = 0;
    DWORD err = 0;
    BOOL ok = WriteFile(g_hPipe, &header, sizeof(header), &bytesWritten, &overlapped);
    if (!ok) err = GetLastError();
    HostLog("INFO", "WriteFile returned: ok=%d, err=%lu, bytesWritten=%lu", ok, err, bytesWritten);
    if (!ok)
    {
        if (err == ERROR_IO_PENDING)
        {
            // Wait for write to complete with a 2-second timeout
            DWORD waitRes = WaitForSingleObject(overlapped.hEvent, 2000);
            HostLog("INFO", "WaitForSingleObject returned: %lu", waitRes);
            if (waitRes == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(g_hPipe, &overlapped, &bytesWritten, FALSE);
                HostLog("INFO", "GetOverlappedResult returned: ok=%d, bytesWritten=%lu", ok, bytesWritten);
            }
            else
            {
                HostLog("ERROR", "WriteFile timeout/failed in SendPipeMessage. waitRes=%lu", waitRes);
                CancelIoEx(g_hPipe, &overlapped);
                ok = FALSE;
            }
        }
        else
        {
            HostLog("ERROR", "WriteFile failed in SendPipeMessage: %lu", err);
            ok = FALSE;
        }
    }

    CloseHandle(overlapped.hEvent);
    HostLog("INFO", "SendPipeMessage: type=%d end, ok=%d, bytesWritten=%lu", type, ok, bytesWritten);
    return (ok && bytesWritten == sizeof(header));
}

void ReadPipeLoop()
{
    HostLog("INFO", "Background read pipe loop started.");
    
    HANDLE hReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hReadEvent)
    {
        HostLog("ERROR", "CreateEventW failed in ReadPipeLoop.");
        g_running = false;
        return;
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = hReadEvent;

    while (g_running)
    {
        MessageHeader header = {};
        DWORD bytesRead = 0;
        ResetEvent(hReadEvent);

        HostLog("INFO", "ReadPipeLoop: Calling ReadFile...");
        DWORD err = 0;
        BOOL ok = ReadFile(g_hPipe, &header, sizeof(header), &bytesRead, &overlapped);
        if (!ok) err = GetLastError();
        HostLog("INFO", "ReadPipeLoop: ReadFile returned ok=%d, err=%lu, bytesRead=%lu", ok, err, bytesRead);
        if (!ok)
        {
            if (err == ERROR_IO_PENDING)
            {
                // Wait for read to complete or shutdown signal
                while (g_running)
                {
                    DWORD waitRes = WaitForSingleObject(hReadEvent, 500);
                    if (waitRes == WAIT_OBJECT_0)
                    {
                        ok = GetOverlappedResult(g_hPipe, &overlapped, &bytesRead, FALSE);
                        HostLog("INFO", "ReadPipeLoop: GetOverlappedResult returned ok=%d, bytesRead=%lu", ok, bytesRead);
                        break;
                    }
                    else if (waitRes == WAIT_TIMEOUT)
                    {
                        continue;
                    }
                    else
                    {
                        HostLog("ERROR", "WaitForSingleObject failed in ReadPipeLoop: %lu", GetLastError());
                        CancelIoEx(g_hPipe, &overlapped);
                        ok = FALSE;
                        break;
                    }
                }
                if (!g_running && ok == FALSE)
                {
                    CancelIoEx(g_hPipe, &overlapped);
                }
            }
            else
            {
                HostLog("INFO", "ReadFile failed in ReadPipeLoop: %lu. Exiting loop.", err);
                g_running = false;
                break;
            }
        }

        if (!g_running) break;

        if (ok && bytesRead == sizeof(header))
        {
            if (header.magic == MSG_MAGIC &&
                header.sessionNonceHi == g_nonceHi &&
                header.sessionNonceLo == g_nonceLo)
            {
                if (header.type == MSG_SHUTDOWN || header.type == MSG_CANCEL_SCAN)
                {
                    HostLog("INFO", "Received shutdown/cancel message (type=%d) from CP.", header.type);
                    g_running = false;
                    break;
                }
            }
        }
        else
        {
            HostLog("INFO", "ReadFile returned incomplete or no bytes: %lu. Exiting loop.", bytesRead);
            g_running = false;
            break;
        }
    }

    CloseHandle(hReadEvent);
    HostLog("INFO", "Background read pipe loop exiting.");
}

std::wstring GetModelPath(const std::wstring& filename)
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        PathAppendW(path, L"HomeFaceLogon");
        PathAppendW(path, L"models");
        PathAppendW(path, filename.c_str());
        return path;
    }
    return L"";
}

std::string WStringToUTF8(const std::wstring& wstr)
{
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

bool LoadFaceTemplate(std::vector<float>& featureVec)
{
    HostLog("INFO", "Loading enrolled face template...");
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        HostLog("ERROR", "SHGetFolderPathW failed.");
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    PathAppendW(path, L"face.bin");

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        HostLog("ERROR", "Failed to open face.bin for reading.");
        return false;
    }

    FaceFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != HFLF_MAGIC || header.schemaVersion != 1)
    {
        HostLog("ERROR", "Invalid face.bin header magic/version.");
        return false;
    }

    if (header.protectedBlobSize > 65536)
    {
        HostLog("ERROR", "Invalid face.bin protected blob size.");
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
        HostLog("ERROR", "CryptUnprotectData failed: %lu", GetLastError());
        return false;
    }

    if (output.cbData != 128 * sizeof(float))
    {
        HostLog("ERROR", "Decrypted template size mismatch: %lu bytes.", output.cbData);
        LocalFree(output.pbData);
        return false;
    }

    featureVec.resize(128);
    memcpy(featureVec.data(), output.pbData, output.cbData);
    LocalFree(output.pbData);
    HostLog("INFO", "Face template loaded and decrypted successfully.");
    return true;
}

struct HostConfig {
    double matchThreshold = 0.363;
    int requiredMatches = 3;
    int windowSize = 5;
    int scanTimeoutMs = 10000;
    bool livenessEnabled = false;
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

static int ExtractInt(const std::string& content, const std::string& key, int defaultValue)
{
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return defaultValue;
    pos = content.find(":", pos);
    if (pos == std::string::npos) return defaultValue;
    size_t valPos = content.find_first_not_of(" \t\r\n", pos + 1);
    if (valPos == std::string::npos) return defaultValue;
    try {
        return std::stoi(content.substr(valPos));
    } catch(...) {
        return defaultValue;
    }
}
static bool ExtractBool(const std::string& content, const std::string& key, bool defaultValue)
{
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return defaultValue;
    pos = content.find(":", pos);
    if (pos == std::string::npos) return defaultValue;
    size_t valPos = content.find_first_not_of(" \t\r\n", pos + 1);
    if (valPos == std::string::npos) return defaultValue;
    if (content.compare(valPos, 4, "true") == 0) return true;
    if (content.compare(valPos, 5, "false") == 0) return false;
    return defaultValue;
}

static constexpr int kCameraWidth = 640;
static constexpr int kCameraHeight = 480;

// MSMF/DirectShow require COM on the thread that opens and uses VideoCapture.
struct ComApartment
{
    HRESULT hr;

    ComApartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment()
    {
        if (hr == S_OK)
        {
            CoUninitialize();
        }
    }

    bool Ok() const
    {
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
};

static void ConfigureCameraProperties(cv::VideoCapture& cap)
{
    cap.set(cv::CAP_PROP_FRAME_WIDTH, kCameraWidth);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, kCameraHeight);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
}

static bool WarmUpCamera(cv::VideoCapture& cap, int maxAttempts = 30, int delayMs = 100)
{
    ULONGLONG warmupStart = GetTickCount64();
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        if (cap.grab())
        {
            HostLog("INFO", "Camera warm-up succeeded: attempts=%d, elapsed=%llums", attempt + 1, GetTickCount64() - warmupStart);
            return true;
        }
        Sleep(delayMs);
    }
    HostLog("WARNING", "Camera warm-up failed: attempts=%d, elapsed=%llums", maxAttempts, GetTickCount64() - warmupStart);
    return false;
}

// Camera open and retry logic runs entirely on a single worker thread to avoid
// DirectShow device lock contention.  The main thread waits for the result with
// a hard overall timeout.
//
// Previous design (v0.3.0) spawned a new worker per retry attempt and detached
// timed-out threads.  Detached threads held the camera device lock, causing ALL
// subsequent retry workers to also time out.  This new design ensures only one
// thread ever touches the camera at any given time.
struct CameraOpenResult
{
    cv::VideoCapture cap;
    int width = 0;
    int height = 0;
    bool success = false;
};

static void CameraWorkerThread(int cameraIndex, std::shared_ptr<CameraOpenResult> result,
                               std::shared_ptr<std::atomic<bool>> done)
{
    ComApartment com;
    if (!com.Ok())
    {
        HostLog("ERROR", "Camera worker: COM init failed.");
        done->store(true);
        return;
    }

    const int MAX_RETRIES = 5;
    const int RETRY_DELAY_MS = 1000;

    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt)
    {
        const bool allowFallback = (attempt >= MAX_RETRIES - 1);
        HostLog("INFO", "Attempting to open camera index %d (attempt %d/%d, allowFallback=%s)...",
                cameraIndex, attempt, MAX_RETRIES, allowFallback ? "true" : "false");

        ULONGLONG backendStart = GetTickCount64();

        // Try DirectShow first (usually fastest)
        HostLog("INFO", "Attempting to open camera index %d using CAP_DSHOW (allowFallback=%s)...",
                cameraIndex, allowFallback ? "true" : "false");
        result->cap.open(cameraIndex, cv::CAP_DSHOW);
        ULONGLONG elapsed = GetTickCount64() - backendStart;
        HostLog("INFO", "CAP_DSHOW open finished: opened=%d, elapsed=%llums",
                result->cap.isOpened(), elapsed);

        // Fallback to MSMF if DirectShow failed
        if (!result->cap.isOpened() && allowFallback)
        {
            backendStart = GetTickCount64();
            HostLog("INFO", "DirectShow failed, trying Media Foundation (CAP_MSMF)...");
            result->cap.open(cameraIndex, cv::CAP_MSMF);
            HostLog("INFO", "CAP_MSMF open finished: opened=%d, elapsed=%llums",
                    result->cap.isOpened(), GetTickCount64() - backendStart);
        }

        // Fallback to default backend
        if (!result->cap.isOpened() && allowFallback)
        {
            backendStart = GetTickCount64();
            HostLog("INFO", "Media Foundation failed, trying default backend...");
            result->cap.open(cameraIndex);
            HostLog("INFO", "Default backend open finished: opened=%d, elapsed=%llums",
                    result->cap.isOpened(), GetTickCount64() - backendStart);
        }

        if (result->cap.isOpened())
        {
            ConfigureCameraProperties(result->cap);

            result->width = static_cast<int>(result->cap.get(cv::CAP_PROP_FRAME_WIDTH));
            result->height = static_cast<int>(result->cap.get(cv::CAP_PROP_FRAME_HEIGHT));
            if (result->width <= 0 || result->height <= 0)
            {
                result->width = kCameraWidth;
                result->height = kCameraHeight;
            }

            int warmAttempts = (attempt < MAX_RETRIES) ? 10 : 30;
            if (WarmUpCamera(result->cap, warmAttempts))
            {
                HostLog("INFO", "Camera opened and streaming successfully (%dx%d).",
                        result->width, result->height);
                result->success = true;
                done->store(true);
                return;
            }

            HostLog("WARNING", "Camera opened but warm-up failed. Releasing for retry...");
            result->cap.release();
        }

        if (attempt < MAX_RETRIES)
        {
            HostLog("INFO", "Camera device busy or resuming. Waiting %d ms before retry...",
                    RETRY_DELAY_MS);
            Sleep(RETRY_DELAY_MS);
        }
    }

    HostLog("ERROR", "Failed to open camera device after all retries.");
    done->store(true);
}

// Opens the camera using a single dedicated worker thread.
// The main thread polls for completion with an overall timeout.
//
// IMPORTANT: We cannot use a simple GetTickCount64() deadline because the tick
// counter keeps running during sleep/hibernate.  If the system sleeps for hours,
// the deadline would have already passed on resume, causing instant false timeout.
// Instead we accumulate only "awake" elapsed time between polls.  Any gap larger
// than 2 seconds between consecutive polls is assumed to be a sleep interval and
// is excluded from the elapsed total.
static bool OpenCameraWithRetry(cv::VideoCapture& cap, int cameraIndex, int& width, int& height)
{
    const DWORD OVERALL_TIMEOUT_MS = 25000;
    const DWORD POLL_INTERVAL_MS = 100;
    // Any single poll gap larger than this is assumed to be a sleep/resume event.
    const DWORD SLEEP_GAP_THRESHOLD_MS = 2000;

    auto result = std::make_shared<CameraOpenResult>();
    auto done = std::make_shared<std::atomic<bool>>(false);

    std::thread worker(CameraWorkerThread, cameraIndex, result, done);

    ULONGLONG awakeElapsedMs = 0;
    ULONGLONG lastPollTick = GetTickCount64();

    while (!done->load())
    {
        Sleep(POLL_INTERVAL_MS);

        ULONGLONG now = GetTickCount64();
        ULONGLONG gap = now - lastPollTick;
        lastPollTick = now;

        if (gap < SLEEP_GAP_THRESHOLD_MS)
        {
            awakeElapsedMs += gap;
        }
        else
        {
            // Large gap detected — system likely resumed from sleep.
            // Reset elapsed time so the worker gets a fresh chance.
            HostLog("INFO", "Sleep/resume detected (gap=%llums). Resetting camera timeout.", gap);
            awakeElapsedMs = 0;
        }

        if (awakeElapsedMs >= OVERALL_TIMEOUT_MS)
        {
            HostLog("WARNING", "Camera open overall timeout (%lu ms awake time). Worker thread still running.",
                    OVERALL_TIMEOUT_MS);
            worker.detach();
            return false;
        }
    }

    worker.join();

    if (result->success)
    {
        cap = std::move(result->cap);
        width = result->width;
        height = result->height;
        return true;
    }
    return false;
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
    config.requiredMatches = ExtractInt(content, "requiredMatches", 3);
    config.windowSize = ExtractInt(content, "windowSize", 5);
    config.scanTimeoutMs = ExtractInt(content, "scanTimeoutMs", 10000);
    config.livenessEnabled = ExtractBool(content, "livenessEnabled", false);

    if (config.matchThreshold <= 0.0 || config.matchThreshold > 1.0) config.matchThreshold = 0.363;
    if (config.windowSize < 1) config.windowSize = 5;
    if (config.windowSize > 30) config.windowSize = 30;
    if (config.requiredMatches < 1) config.requiredMatches = 3;
    if (config.requiredMatches > config.windowSize) config.requiredMatches = config.windowSize;
    if (config.scanTimeoutMs < 1000) config.scanTimeoutMs = 10000;
    if (config.scanTimeoutMs > 30000) config.scanTimeoutMs = 30000;
    return true;
}

int main(int argc, char* argv[])
{
    HostLog("INFO", "FaceLogonHost helper process starting...");

    // Parse arguments
    int cameraIndex = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc)
        {
            std::string pName = argv[++i];
            g_pipeName = std::wstring(pName.begin(), pName.end());
        }
        else if (strcmp(argv[i], "--nonce") == 0 && i + 1 < argc)
        {
            std::string nonceStr = argv[++i];
            if (nonceStr.length() == 32)
            {
                std::string hiStr = nonceStr.substr(0, 16);
                std::string loStr = nonceStr.substr(16, 16);
                try {
                    g_nonceHi = std::stoull(hiStr, nullptr, 16);
                    g_nonceLo = std::stoull(loStr, nullptr, 16);
                } catch(...) {
                    HostLog("ERROR", "Invalid nonce hex format.");
                    return 1;
                }
            }
        }
        else if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc)
        {
            cameraIndex = atoi(argv[++i]);
        }
    }

    if (g_pipeName.empty())
    {
        HostLog("ERROR", "--pipe parameter required.");
        return 1;
    }

    HostLog("INFO", "Connecting to pipe: %ls", g_pipeName.c_str());

    // Wait for Named Pipe to be available and connect
    int retries = 10;
    while (retries-- > 0)
    {
        g_hPipe = CreateFileW(
            g_pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, // Add overlapped flag
            nullptr
        );

        if (g_hPipe != INVALID_HANDLE_VALUE)
        {
            break;
        }

        if (GetLastError() == ERROR_PIPE_BUSY)
        {
            WaitNamedPipeW(g_pipeName.c_str(), 1000);
        }
        else
        {
            Sleep(500);
        }
    }

    if (g_hPipe == INVALID_HANDLE_VALUE)
    {
        HostLog("ERROR", "Failed to connect to Named Pipe: %lu", GetLastError());
        return 1;
    }

    HostLog("INFO", "Connected to Named Pipe successfully.");

    // Start background thread to read messages from CP
    std::thread readThread(ReadPipeLoop);

    // Parallel startup: face template, camera, and ONNX models are independent.
    std::vector<float> templateVec;
    bool templateOk = false;
    cv::VideoCapture cap;
    bool cameraOk = false;
    int camWidth = kCameraWidth;
    int camHeight = kCameraHeight;
    cv::Ptr<cv::FaceDetectorYN> detector;
    cv::Ptr<cv::FaceRecognizerSF> recognizer;
    bool modelsOk = false;

    if (g_running)
    {
        HostLog("INFO", "Starting synchronous initialization (template, models, camera)...");

        ComApartment com;
        if (!com.Ok())
        {
            HostLog("ERROR", "Failed to initialize COM for camera access.");
            SendPipeMessage(MSG_CAMERA_ERROR);
            g_running = false;
        }
        else
        {
            // 1. Load enrolled face template
            templateOk = LoadFaceTemplate(templateVec);
            if (!templateOk)
            {
                HostLog("ERROR", "Failed to load face template.");
            }

            // 2. Load face models
            if (templateOk)
            {
                HostLog("INFO", "Resolving model paths...");
                std::wstring wModelDet = GetModelPath(L"face_detection_yunet_2023mar.onnx");
                std::wstring wModelRec = GetModelPath(L"face_recognition_sface_2021dec.onnx");
                std::string modelDet = WStringToUTF8(wModelDet);
                std::string modelRec = WStringToUTF8(wModelRec);

                HostLog("INFO", "Model Det Path: %s", modelDet.c_str());
                HostLog("INFO", "Model Rec Path: %s", modelRec.c_str());

                if (modelDet.empty() || modelRec.empty() ||
                    !PathFileExistsW(wModelDet.c_str()) || !PathFileExistsW(wModelRec.c_str()))
                {
                    HostLog("ERROR", "One or more ONNX model files are missing from ProgramData.");
                }
                else
                {
                    try
                    {
                        HostLog("INFO", "Initializing face models at %dx%d...", kCameraWidth, kCameraHeight);
                        detector = cv::FaceDetectorYN::create(
                            modelDet, "", cv::Size(kCameraWidth, kCameraHeight), 0.9f, 0.3f, 5000);
                        recognizer = cv::FaceRecognizerSF::create(modelRec, "");
                        modelsOk = true;
                        HostLog("INFO", "ONNX models initialized successfully.");
                    }
                    catch (const cv::Exception& e)
                    {
                        HostLog("ERROR", "OpenCV exception during model creation: %s", e.what());
                    }
                }
            }

            // 3. Open camera backend
            if (templateOk && modelsOk)
            {
                cameraOk = OpenCameraWithRetry(cap, cameraIndex, camWidth, camHeight);
                if (!cameraOk)
                {
                    HostLog("ERROR", "Failed to open camera device after all retries.");
                }
            }

            // Signal status to credential provider
            if (!templateOk)
            {
                HostLog("ERROR", "Signalling model error (face template).");
                SendPipeMessage(MSG_MODEL_ERROR);
                g_running = false;
            }
            else if (!modelsOk)
            {
                HostLog("ERROR", "Signalling model error (ONNX).");
                SendPipeMessage(MSG_MODEL_ERROR);
                g_running = false;
            }
            else if (!cameraOk)
            {
                SendPipeMessage(MSG_CAMERA_ERROR);
                g_running = false;
            }
        }
    }

    cv::Mat templateFeature;
    if (g_running && templateOk)
    {
        templateFeature = cv::Mat(1, 128, CV_32F, templateVec.data()).clone();
    }

    if (g_running && cameraOk)
    {
        HostLog("INFO", "Signalling status: searching for face.");
        SendPipeMessage(MSG_STATUS);
    }

    HostConfig config;
    LoadHostConfig(config);
    HostLog("INFO", "Host config: matchThreshold=%f, requiredMatches=%d, windowSize=%d, scanTimeoutMs=%d, livenessEnabled=%s",
            config.matchThreshold, config.requiredMatches, config.windowSize, config.scanTimeoutMs, config.livenessEnabled ? "true" : "false");

    int framesCount = 0;
    ULONGLONG startTime = GetTickCount64();

    cv::Mat prevGray;
    cv::Size lastDetectorInputSize;
    std::vector<bool> matchHistory;
    matchHistory.reserve(static_cast<size_t>(config.windowSize));

    // CLAHE (Contrast Limited Adaptive Histogram Equalization) normalizes
    // illumination before face recognition, making scores much more stable
    // under varying lighting conditions (desk lamp, window light, night, etc.).
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    HostLog("INFO", "CLAHE illumination normalizer initialized (clipLimit=2.0, tileGrid=8x8).");

    HostLog("INFO", "Starting main capture and recognition loop...");
    while (g_running && cameraOk)
    {
        // Check scanning timeout
        if (GetTickCount64() - startTime > static_cast<ULONGLONG>(config.scanTimeoutMs))
        {
            HostLog("INFO", "Scan timeout reached (%d ms). Exiting with no match.", config.scanTimeoutMs);
            SendPipeMessage(MSG_NO_MATCH);
            break;
        }

        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
        {
            HostLog("ERROR", "Empty frame grabbed.");
            SendPipeMessage(MSG_CAMERA_ERROR);
            break;
        }

        framesCount++;

        // 1. Motion Detection (Liveness check). Skip all grayscale/blur work on the fast path.
        bool motionDetected = true;
        if (config.livenessEnabled)
        {
            cv::Mat gray;
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
            cv::GaussianBlur(gray, gray, cv::Size(21, 21), 0);

            motionDetected = false;
            if (!prevGray.empty())
            {
                cv::Mat diff;
                cv::absdiff(prevGray, gray, diff);
                cv::Mat thresh;
                cv::threshold(diff, thresh, 15, 255, cv::THRESH_BINARY);
                double motionPixels = cv::countNonZero(thresh);
                double totalPixels = frame.cols * frame.rows;
                // If at least 0.5% of pixels changed, motion is detected
                if (motionPixels > totalPixels * 0.005)
                {
                    motionDetected = true;
                }
            }
            else
            {
                // Always allow the first frame to establish a baseline
                motionDetected = true;
            }
            prevGray = gray.clone();
        }

        bool frameMatched = false;

        if (!config.livenessEnabled || motionDetected)
        {
            // Apply CLAHE illumination normalization before face detection/recognition.
            // Convert BGR -> LAB, equalize L channel, convert back.
            // This makes recognition scores much more stable under varying lighting.
            cv::Mat normalizedFrame;
            {
                cv::Mat lab;
                cv::cvtColor(frame, lab, cv::COLOR_BGR2Lab);
                std::vector<cv::Mat> labChannels;
                cv::split(lab, labChannels);
                clahe->apply(labChannels[0], labChannels[0]);
                cv::merge(labChannels, lab);
                cv::cvtColor(lab, normalizedFrame, cv::COLOR_Lab2BGR);
            }

            cv::Mat faces;
            if (lastDetectorInputSize != normalizedFrame.size())
            {
                detector->setInputSize(normalizedFrame.size());
                lastDetectorInputSize = normalizedFrame.size();
            }
            detector->detect(normalizedFrame, faces);

            if (faces.rows > 0)
            {
                for (int i = 0; i < faces.rows; ++i)
                {
                    cv::Mat aligned;
                    recognizer->alignCrop(normalizedFrame, faces.row(i), aligned);
                    cv::Mat feature;
                    recognizer->feature(aligned, feature);

                    double score = recognizer->match(feature, templateFeature, cv::FaceRecognizerSF::DisType::FR_COSINE);
                    if (score >= config.matchThreshold)
                    {
                        HostLog("INFO", "Frame %d: Face[%d] matched (score=%f >= threshold=%f)", framesCount, i, score, config.matchThreshold);
                        frameMatched = true;
                        break;
                    }
                    else
                    {
                        // Log near-misses to help diagnose threshold tuning
                        HostLog("INFO", "Frame %d: Face[%d] below threshold (score=%f < threshold=%f)",
                                framesCount, i, score, config.matchThreshold);
                    }
                }
            }
        }

        // 2. Sliding Window Verification
        matchHistory.push_back(frameMatched);
        if (matchHistory.size() > static_cast<size_t>(config.windowSize))
        {
            matchHistory.erase(matchHistory.begin());
        }

        int matchCount = 0;
        for (bool m : matchHistory)
        {
            if (m) matchCount++;
        }

        if (matchCount >= config.requiredMatches)
        {
            HostLog("INFO", "FACE MATCHED CONSECUTIVE SUCCESS (%d/%d in window of %d)", 
                    matchCount, config.requiredMatches, config.windowSize);
            SendPipeMessage(MSG_MATCHED);
            g_running = false;
            break;
        }

        if (framesCount % 30 == 0)
        {
            HostLog("INFO", "Successfully processed %d frames. Sending status keepalive.", framesCount);
            SendPipeMessage(MSG_STATUS);
        }

        // VideoCapture already paces live camera input; an extra Sleep here delays sign-in.
    }

    HostLog("INFO", "Exiting capture loop. Stopping camera.");
    g_running = false;
    
    // Close pipe handle to unblock readThread if it's waiting
    if (g_hPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_hPipe);
        g_hPipe = INVALID_HANDLE_VALUE;
    }

    if (readThread.joinable())
    {
        readThread.join();
    }

    HostLog("INFO", "FaceLogonHost exited successfully.");
    return 0;
}
