#pragma once
// Shared camera open logic for FaceLogonHost and FaceLogonSetup.
//
// Measured on a Logicool C270 (see docs/camera-open-tuning.md):
//   DSHOW open + late set(W/H)  : ~2.4 s to first frame, ~15 fps
//   MSMF  open with params      : ~1.1 s to first frame, ~30 fps
// Production logs also showed DSHOW opens taking up to ~13 s after resume,
// while the first frame almost always arrived immediately once opened.
//
// Strategy (camera-agnostic):
//  * The configured index is a DirectShow index (the setup UI enumerates
//    DirectShow). MSMF is used only when the same friendly name can be mapped
//    unambiguously to an MSMF index, so multi-camera / IR / virtual-camera
//    systems never silently switch to a different device.
//  * Each attempt tries the backends in preference order. The backend that
//    last worked for this camera is remembered in a small cache file and
//    tried first next time.
//  * Resolution is passed at open time (MSMF) or only changed when the
//    native mode is larger than needed (DSHOW), avoiding a graph rebuild.
//  * The open attempt succeeds only after a real non-empty frame arrives.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <dshow.h>
#include <opencv2/opencv.hpp>
#include "camera_preference.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace hfl_camera
{
using LogFn = void (*)(const char* level, const char* format, ...);

constexpr int kTargetWidth = 640;
constexpr int kTargetHeight = 480;
// DSHOW native modes up to this width are used as-is (no graph rebuild).
constexpr int kMaxNativeWidth = 960;

inline ULONGLONG NowMs()
{
    ULONGLONG unbiased100ns = 0;
    if (QueryUnbiasedInterruptTime(&unbiased100ns)) return unbiased100ns / 10000ULL;
    return GetTickCount64();
}

inline std::string ToUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Must run before OpenCV loads the MSMF plugin. Hardware transforms are a
// well-known cause of multi-second MSMF opens on some drivers; we only need
// 640x480 BGR, which software conversion handles easily.
inline void ConfigureOpenCvCameraEnvironment()
{
    _putenv_s("OPENCV_VIDEOIO_MSMF_ENABLE_HW_TRANSFORMS", "0");
}

inline std::vector<std::wstring> EnumerateDshowNames()
{
    std::vector<std::wstring> names;
    ICreateDevEnum* devEnum = nullptr;
    IEnumMoniker* monikers = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum))) || !devEnum)
        return names;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) == S_OK && monikers)
    {
        IMoniker* moniker = nullptr;
        while (monikers->Next(1, &moniker, nullptr) == S_OK)
        {
            std::wstring name;
            IPropertyBag* bag = nullptr;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) && bag)
            {
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
                    name = v.bstrVal;
                VariantClear(&v);
                bag->Release();
            }
            names.push_back(name);
            moniker->Release();
        }
        monikers->Release();
    }
    devEnum->Release();
    return names;
}

// Same enumeration OpenCV's MSMF backend uses to resolve an index.
inline std::vector<std::wstring> EnumerateMsmfNames()
{
    std::vector<std::wstring> names;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return names;
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) && attrs &&
        SUCCEEDED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
    {
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count)))
        {
            for (UINT32 i = 0; i < count; ++i)
            {
                std::wstring name;
                WCHAR* raw = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &raw, &len)) && raw)
                {
                    name.assign(raw, len);
                    CoTaskMemFree(raw);
                }
                names.push_back(name);
                devices[i]->Release();
            }
            CoTaskMemFree(devices);
        }
    }
    if (attrs) attrs->Release();
    MFShutdown();
    return names;
}

// Returns the MSMF index for the DSHOW device, or -1 when the mapping is not
// provably the same physical device (duplicate names, missing, unnamed).
inline int MapDshowToMsmf(int dshowIndex, std::wstring* nameOut)
{
    auto dshow = EnumerateDshowNames();
    if (dshowIndex < 0 || dshowIndex >= static_cast<int>(dshow.size())) return -1;
    const std::wstring& name = dshow[static_cast<size_t>(dshowIndex)];
    if (nameOut) *nameOut = name;
    if (name.empty()) return -1;
    int dshowSame = 0;
    for (const auto& n : dshow) if (n == name) ++dshowSame;
    auto msmf = EnumerateMsmfNames();
    int found = -1, msmfSame = 0;
    for (size_t i = 0; i < msmf.size(); ++i)
    {
        if (msmf[i] == name)
        {
            ++msmfSame;
            if (found < 0) found = static_cast<int>(i);
        }
    }
    if (dshowSame != 1 || msmfSame != 1) return -1;
    return found;
}

inline Preference LoadPreference(const std::wstring& cachePath, const std::wstring& deviceName)
{
    Preference p;
    if (cachePath.empty()) return p;
    std::ifstream f(cachePath);
    std::string backend, name, streak;
    if (!std::getline(f, backend) || !std::getline(f, name)) return p;
    if (name != ToUtf8(deviceName)) return p; // different camera: ignore stale cache
    if (backend == "DSHOW") { p.backend = Backend::DSHOW; p.known = true; }
    else if (backend == "MSMF") { p.backend = Backend::MSMF; p.known = true; }
    if (p.known && std::getline(f, streak))
    {
        const int value = std::atoi(streak.c_str());
        p.streak = value > 0 ? value : 0;
    }
    return p;
}

inline void SavePreference(const std::wstring& cachePath, const std::wstring& deviceName, Backend b, int streak)
{
    if (cachePath.empty()) return;
    std::ofstream f(cachePath, std::ios::trunc);
    if (f) f << BackendName(b) << "\n" << ToUtf8(deviceName) << "\n" << streak << "\n";
}

// MSMF normally delivers its first frame in ~0.9-1.05 s. A session opened
// while another app holds the camera never delivers frames, even after the
// device is released, so waiting longer than this only delays the re-open.
constexpr DWORD kMsmfFirstFrameTimeoutMs = 3000;

// Opens one backend and waits for a real frame. Leaves cap released on failure.
// stalled is set when the device opened but produced no frame.
inline bool OpenBackendAndWaitForFrame(cv::VideoCapture& cap, Backend backend, int index,
                                       DWORD firstFrameTimeoutMs, LogFn log, int& width, int& height,
                                       bool* stalled = nullptr)
{
    if (stalled) *stalled = false;
    if (backend == Backend::MSMF && firstFrameTimeoutMs > kMsmfFirstFrameTimeoutMs)
        firstFrameTimeoutMs = kMsmfFirstFrameTimeoutMs;
    const ULONGLONG t0 = NowMs();
    try
    {
        if (backend == Backend::MSMF)
        {
            cap.open(index, cv::CAP_MSMF,
                     { cv::CAP_PROP_FRAME_WIDTH, kTargetWidth,
                       cv::CAP_PROP_FRAME_HEIGHT, kTargetHeight,
                       cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_NONE });
        }
        else
        {
            cap.open(index, cv::CAP_DSHOW);
        }
    }
    catch (const cv::Exception& e)
    {
        log("WARNING", "%s open threw: %s", BackendName(backend), e.what());
        cap.release();
        return false;
    }
    const ULONGLONG tOpen = NowMs();
    log("INFO", "%s open (index %d): opened=%d, elapsed=%llums", BackendName(backend), index,
        cap.isOpened() ? 1 : 0, tOpen - t0);
    if (!cap.isOpened())
    {
        cap.release();
        return false;
    }

    if (backend == Backend::DSHOW)
    {
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
        const int nativeW = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        // Changing the format rebuilds the DirectShow graph (~0.8 s on C270),
        // so only do it when the default mode is unnecessarily large.
        if (nativeW <= 0 || nativeW > kMaxNativeWidth)
        {
            cap.set(cv::CAP_PROP_FRAME_WIDTH, kTargetWidth);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, kTargetHeight);
            log("INFO", "DSHOW native width %d too large; requested %dx%d (elapsed=%llums)",
                nativeW, kTargetWidth, kTargetHeight, NowMs() - tOpen);
        }
    }

    cv::Mat frame;
    const ULONGLONG tWait = NowMs();
    int reads = 0;
    while (NowMs() - tWait < firstFrameTimeoutMs)
    {
        ++reads;
        if (cap.read(frame) && !frame.empty()) break;
        frame.release();
        Sleep(30);
    }
    if (frame.empty())
    {
        log("WARNING", "%s opened but no frame within %lums (reads=%d). Releasing.",
            BackendName(backend), firstFrameTimeoutMs, reads);
        cap.release();
        if (stalled) *stalled = true;
        return false;
    }
    width = frame.cols;
    height = frame.rows;
    log("INFO", "%s first frame: %dx%d, reads=%d, firstFrame=%llums, total=%llums",
        BackendName(backend), width, height, reads, NowMs() - tWait, NowMs() - t0);
    return true;
}

struct OpenOptions
{
    int maxAttempts = 6;
    DWORD retryDelayMs = 1000;
    DWORD firstFrameTimeoutMs = 5000;
    std::wstring preferenceCachePath; // empty = no cache
    // Callers can stop retrying early (e.g. user cancelled the scan).
    bool (*shouldContinue)() = nullptr;
};

// Opens the configured DirectShow camera index using the fastest backend that
// is known to address the same physical device.
inline bool OpenCamera(cv::VideoCapture& cap, int dshowIndex, const OpenOptions& opt, LogFn log,
                       int& width, int& height)
{
    std::wstring deviceName;
    const ULONGLONG tMap = NowMs();
    const int msmfIndex = MapDshowToMsmf(dshowIndex, &deviceName);
    const Preference pref = LoadPreference(opt.preferenceCachePath, deviceName);
    log("INFO", "Camera '%s' dshowIndex=%d msmfIndex=%d preferred=%s%s streak=%d (enum=%llums)",
        ToUtf8(deviceName).c_str(), dshowIndex, msmfIndex, BackendName(pref.backend),
        pref.known ? " (cached)" : " (default)", pref.streak, NowMs() - tMap);

    std::vector<std::pair<Backend, int>> order;
    if (msmfIndex >= 0 && pref.backend == Backend::MSMF) order.push_back({ Backend::MSMF, msmfIndex });
    order.push_back({ Backend::DSHOW, dshowIndex });
    if (msmfIndex >= 0 && pref.backend != Backend::MSMF) order.push_back({ Backend::MSMF, msmfIndex });

    for (int attempt = 1; attempt <= opt.maxAttempts; ++attempt)
    {
        if (opt.shouldContinue && !opt.shouldContinue()) return false;
        for (const auto& candidate : order)
        {
            if (opt.shouldContinue && !opt.shouldContinue()) return false;
            log("INFO", "Camera open attempt %d/%d via %s...", attempt, opt.maxAttempts, BackendName(candidate.first));
            bool stalled = false;
            bool opened = OpenBackendAndWaitForFrame(cap, candidate.first, candidate.second,
                                                     opt.firstFrameTimeoutMs, log, width, height, &stalled);
            // A stalled MSMF session is typically one opened while another app
            // held the camera. Re-open MSMF once before falling back, so a
            // just-released camera does not count against MSMF.
            if (!opened && stalled && candidate.first == Backend::MSMF &&
                (!opt.shouldContinue || opt.shouldContinue()))
            {
                log("INFO", "MSMF stalled; re-opening MSMF once before falling back.");
                opened = OpenBackendAndWaitForFrame(cap, candidate.first, candidate.second,
                                                    opt.firstFrameTimeoutMs, log, width, height);
            }
            if (opened)
            {
                const Preference next = NextPreference(pref, msmfIndex >= 0, candidate.first);
                if (!pref.known || next.backend != pref.backend || next.streak != pref.streak)
                    SavePreference(opt.preferenceCachePath, deviceName, next.backend, next.streak);
                return true;
            }
        }
        if (attempt < opt.maxAttempts) Sleep(opt.retryDelayMs);
    }
    log("ERROR", "Camera could not be opened with any backend after %d attempts.", opt.maxAttempts);
    return false;
}
} // namespace hfl_camera
