// config.cpp
#include "config.h"
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <fstream>
#include <sstream>

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
