#include <windows.h>
#include <shlwapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <opencv2/opencv.hpp>

#pragma comment(lib, "Shlwapi.lib")

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

// Global state
std::wstring g_pipeName;
uint64_t g_nonceHi = 0;
uint64_t g_nonceLo = 0;
HANDLE g_hPipe = INVALID_HANDLE_VALUE;
std::atomic<bool> g_running{true};

bool SendPipeMessage(MessageType type)
{
    if (g_hPipe == INVALID_HANDLE_VALUE) return false;

    MessageHeader header = {};
    header.magic = MSG_MAGIC;
    header.version = MSG_VERSION;
    header.type = type;
    header.payloadSize = 0;
    header.sessionNonceHi = g_nonceHi;
    header.sessionNonceLo = g_nonceLo;

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(g_hPipe, &header, sizeof(header), &bytesWritten, nullptr);
    if (!ok || bytesWritten != sizeof(header))
    {
        std::cerr << "WriteFile failed: " << GetLastError() << std::endl;
        return false;
    }
    return true;
}

void ReadPipeLoop()
{
    while (g_running)
    {
        MessageHeader header = {};
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(g_hPipe, &header, sizeof(header), &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
        {
            // Pipe disconnected
            std::cout << "Pipe disconnected. Exiting read loop." << std::endl;
            g_running = false;
            break;
        }

        if (bytesRead == sizeof(header))
        {
            if (header.magic == MSG_MAGIC &&
                header.sessionNonceHi == g_nonceHi &&
                header.sessionNonceLo == g_nonceLo)
            {
                if (header.type == MSG_SHUTDOWN || header.type == MSG_CANCEL_SCAN)
                {
                    std::cout << "Received shutdown/cancel message from CP." << std::endl;
                    g_running = false;
                    break;
                }
            }
        }
    }
}

int main(int argc, char* argv[])
{
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
            // Hex parse for Hi/Lo
            if (nonceStr.length() == 32)
            {
                std::string hiStr = nonceStr.substr(0, 16);
                std::string loStr = nonceStr.substr(16, 16);
                try {
                    g_nonceHi = std::stoull(hiStr, nullptr, 16);
                    g_nonceLo = std::stoull(loStr, nullptr, 16);
                } catch(...) {
                    std::cerr << "Invalid nonce hex format." << std::endl;
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
        std::cerr << "Error: --pipe parameter required." << std::endl;
        return 1;
    }

    std::wcout << L"Connecting to pipe: " << g_pipeName << std::endl;

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
            0,
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
        std::cerr << "Failed to connect to Named Pipe: " << GetLastError() << std::endl;
        return 1;
    }

    std::cout << "Connected to Named Pipe." << std::endl;

    // Start background thread to read messages from CP
    std::thread readThread(ReadPipeLoop);

    // Initialize camera
    cv::VideoCapture cap;
    bool cameraOk = false;

    // Try DirectShow first, then fallback
    cap.open(cameraIndex, cv::CAP_DSHOW);
    if (!cap.isOpened())
    {
        cap.open(cameraIndex);
    }

    if (cap.isOpened())
    {
        cameraOk = true;
        std::cout << "Camera initialized successfully." << std::endl;
        SendPipeMessage(MSG_STATUS); // Just an initial status report
    }
    else
    {
        std::cerr << "Failed to open camera index " << cameraIndex << std::endl;
        SendPipeMessage(MSG_CAMERA_ERROR);
        g_running = false;
    }

    // Keep running until shutdown is requested
    // (In later phases, this will run face detection & verification loop)
    int framesCount = 0;
    while (g_running && cameraOk)
    {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
        {
            std::cerr << "Grabbed empty frame." << std::endl;
            SendPipeMessage(MSG_CAMERA_ERROR);
            break;
        }

        framesCount++;
        if (framesCount % 30 == 0)
        {
            std::cout << "Successfully grabbed " << framesCount << " frames." << std::endl;
            // Send STATUS update
            SendPipeMessage(MSG_STATUS);
        }

        // Simulating matching check for this spike phase.
        // Once we integrate real YuNet/SFace in Phase 4, we will perform actual detection here.
        // For Phase 3, we just hold camera and read frames to verify capture doesn't fail.

        Sleep(33); // ~30fps
    }

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

    std::cout << "FaceLogonHost exited." << std::endl;
    return 0;
}
