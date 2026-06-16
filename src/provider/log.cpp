// log.cpp
#include "log.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <ctime>
#include <mutex>

static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;

void LogInit()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile != nullptr) return;

    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        PathAppendW(path, L"HomeFaceLogon");
        CreateDirectoryW(path, nullptr);
        PathAppendW(path, L"logs");
        CreateDirectoryW(path, nullptr);
        PathAppendW(path, L"FaceLogon.log");

        // Rotate log if it exceeds 2MB
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        {
            LARGE_INTEGER size;
            size.LowPart = fad.nFileSizeLow;
            size.HighPart = fad.nFileSizeHigh;
            if (size.QuadPart > 2 * 1024 * 1024) // 2MB
            {
                wchar_t rotatePath[MAX_PATH];
                StringCchCopyW(rotatePath, MAX_PATH, path);
                StringCchCatW(rotatePath, MAX_PATH, L".1");
                DeleteFileW(rotatePath);
                MoveFileW(path, rotatePath);
            }
        }

        _wfopen_s(&g_logFile, path, L"a, ccs=UTF-8");
    }
}

void LogShutdown()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile != nullptr)
    {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

static void LogWrite(const wchar_t* level, const wchar_t* message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile == nullptr)
    {
        wchar_t dbgMsg[2048];
        StringCchPrintfW(dbgMsg, 2048, L"[HomeFaceLogon] [%ls] %ls\n", level, message);
        OutputDebugStringW(dbgMsg);
        return;
    }

    time_t now = time(nullptr);
    tm timeinfo;
    localtime_s(&timeinfo, &now);

    wchar_t timeStr[64];
    wcsftime(timeStr, 64, L"%Y-%m-%d %H:%M:%S", &timeinfo);

    fwprintf(g_logFile, L"[%ls] [%ls] %ls\n", timeStr, level, message);
    fflush(g_logFile);
}

void LogInfo(const wchar_t* format, ...)
{
    if (g_logFile == nullptr) LogInit();

    va_list args;
    va_start(args, format);
    wchar_t message[2048];
    vswprintf_s(message, 2048, format, args);
    va_end(args);

    LogWrite(L"INFO", message);
}

void LogError(const wchar_t* format, ...)
{
    if (g_logFile == nullptr) LogInit();

    va_list args;
    va_start(args, format);
    wchar_t message[2048];
    vswprintf_s(message, 2048, format, args);
    va_end(args);

    LogWrite(L"ERROR", message);
}

std::wstring MaskQualifiedUserName(const std::wstring& name)
{
    size_t backslash = name.find(L'\\');
    std::wstring provider = L"";
    std::wstring userPart = name;
    if (backslash != std::wstring::npos)
    {
        provider = name.substr(0, backslash + 1);
        userPart = name.substr(backslash + 1);
    }

    size_t at = userPart.find(L'@');
    if (at != std::wstring::npos)
    {
        std::wstring mailbox = userPart.substr(0, at);
        std::wstring domain = userPart.substr(at + 1);

        if (mailbox.length() > 2)
        {
            mailbox = mailbox.substr(0, 2) + L"***";
        }
        else
        {
            mailbox = mailbox + L"***";
        }

        size_t dot = domain.find(L'.');
        if (dot != std::wstring::npos && dot > 1)
        {
            domain = domain.substr(0, 2) + L"***" + domain.substr(dot);
        }
        else
        {
            domain = L"***";
        }

        return provider + mailbox + L"@" + domain;
    }
    else
    {
        if (userPart.length() > 3)
        {
            return provider + userPart.substr(0, 3) + L"***";
        }
        return provider + userPart + L"***";
    }
}

std::wstring MaskSid(const std::wstring& sid)
{
    size_t lastDash = sid.rfind(L'-');
    if (lastDash != std::wstring::npos && lastDash > 10)
    {
        std::wstring rid = sid.substr(lastDash);
        return L"S-1-5-21-...-" + rid.substr(1);
    }
    return L"S-1-5-...";
}
