// config.cpp
#include "config.h"
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <fstream>
#include <sstream>
#include <vector>

static std::wstring ExtractStringField(const std::string& content, const std::string& fieldName)
{
    size_t pos = content.find("\"" + fieldName + "\"");
    if (pos == std::string::npos) return L"";

    pos = content.find(":", pos);
    if (pos == std::string::npos) return L"";

    size_t startQuote = content.find("\"", pos);
    if (startQuote == std::string::npos) return L"";

    size_t endQuote = content.find("\"", startQuote + 1);
    if (endQuote == std::string::npos) return L"";

    std::string value = content.substr(startQuote + 1, endQuote - startQuote - 1);
    
    int wlen = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (wlen > 0)
    {
        std::wstring wvalue(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &wvalue[0], wlen);
        return wvalue;
    }
    return L"";
}

static bool ExtractBoolField(const std::string& content, const std::string& fieldName)
{
    size_t pos = content.find("\"" + fieldName + "\"");
    if (pos == std::string::npos) return false;

    pos = content.find(":", pos);
    if (pos == std::string::npos) return false;

    size_t valPos = content.find_first_not_of(" \t\r\n", pos + 1);
    if (valPos == std::string::npos) return false;

    if (content.compare(valPos, 4, "true") == 0) return true;
    return false;
}

bool LoadAppConfig(AppConfig* config)
{
    if (config == nullptr) return false;

    config->targetSid = L"";
    config->enabled = false;

    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    PathAppendW(path, L"config.json");

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    config->targetSid = ExtractStringField(content, "targetSid");
    config->enabled = ExtractBoolField(content, "enabled");

    return true;
}

#pragma comment(lib, "Crypt32.lib")

struct SecretFileHeader {
    uint32_t magic;          // 'HFLO' (0x4F4C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t protectedBlobSize;
};

const uint32_t HFLO_MAGIC = 0x4F4C4648;

bool LoadAndDecryptPassword(PWSTR* ppszPassword)
{
    if (ppszPassword == nullptr) return false;
    *ppszPassword = nullptr;

    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    PathAppendW(path, L"secret.bin");

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    SecretFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (file.gcount() != sizeof(header) || header.magic != HFLO_MAGIC || header.schemaVersion != 1)
    {
        file.close();
        return false;
    }

    std::vector<char> encryptedBlob(header.protectedBlobSize);
    file.read(encryptedBlob.data(), header.protectedBlobSize);
    if (file.gcount() != header.protectedBlobSize)
    {
        file.close();
        return false;
    }
    file.close();

    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(encryptedBlob.data());
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
        return false;
    }

    PWSTR pszPassword = static_cast<PWSTR>(CoTaskMemAlloc(output.cbData));
    if (pszPassword == nullptr)
    {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return false;
    }

    CopyMemory(pszPassword, output.pbData, output.cbData);
    
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);

    *ppszPassword = pszPassword;
    return true;
}

