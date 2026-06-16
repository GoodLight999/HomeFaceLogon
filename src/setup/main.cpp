#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <iostream>
#include <string>
#include <fstream>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Shlwapi.lib")

struct SecretFileHeader {
    uint32_t magic;          // 'HFLO' (0x4F4C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t protectedBlobSize;
};

const uint32_t HFLO_MAGIC = 0x4F4C4648;

bool SaveSecret(const std::wstring& password)
{
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(password.c_str()));
    input.cbData = static_cast<DWORD>((password.length() + 1) * sizeof(wchar_t));

    DATA_BLOB output = {0};
    
    if (!CryptProtectData(
        &input,
        L"HomeFaceLogon MSA password",
        nullptr,
        nullptr,
        nullptr,
        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
        &output
    ))
    {
        std::wcerr << L"CryptProtectData failed: " << GetLastError() << std::endl;
        return false;
    }

    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        LocalFree(output.pbData);
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"secret.bin");

    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open secret.bin for writing." << std::endl;
        LocalFree(output.pbData);
        return false;
    }

    SecretFileHeader header;
    header.magic = HFLO_MAGIC;
    header.schemaVersion = 1;
    header.flags = 0;
    header.protectedBlobSize = output.cbData;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    file.close();

    LocalFree(output.pbData);
    return true;
}

bool SaveConfig(const std::wstring& sid)
{
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    CreateDirectoryW(path, nullptr);
    PathAppendW(path, L"config.json");

    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open())
    {
        std::wcerr << L"Failed to open config.json for writing." << std::endl;
        return false;
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, sid.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Sid(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, sid.c_str(), -1, &utf8Sid[0], len, nullptr, nullptr);

    file << "{\n";
    file << "  \"schemaVersion\": 1,\n";
    file << "  \"enabled\": true,\n";
    file << "  \"targetSid\": \"" << utf8Sid << "\"\n";
    file << "}\n";
    file.close();

    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    std::wstring sid;
    std::wstring password;

    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--sid") == 0 && i + 1 < argc)
        {
            sid = argv[++i];
        }
        else if (_wcsicmp(argv[i], L"--password") == 0 && i + 1 < argc)
        {
            password = argv[++i];
        }
    }

    if (sid.empty())
    {
        std::wcout << L"Enter Target Windows User SID: ";
        std::wcin >> sid;
    }

    if (password.empty())
    {
        std::wcout << L"Enter Microsoft Account Password: ";
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
        
        std::wcin >> password;
        
        SetConsoleMode(hStdin, mode);
        std::wcout << std::endl;
    }

    if (sid.empty() || password.empty())
    {
        std::wcerr << L"Error: SID and Password cannot be empty." << std::endl;
        return 1;
    }

    if (!SaveSecret(password))
    {
        std::wcerr << L"Failed to save encrypted secret." << std::endl;
        SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
        return 1;
    }

    if (!SaveConfig(sid))
    {
        std::wcerr << L"Failed to save configuration." << std::endl;
        SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));
        return 1;
    }

    SecureZeroMemory(&password[0], password.length() * sizeof(wchar_t));

    std::wcout << L"Setup completed successfully. targetSid and secret.bin updated." << std::endl;
    return 0;
}
