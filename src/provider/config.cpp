// config.cpp
#include "config.h"
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <bcrypt.h>

#pragma comment(lib, "Bcrypt.lib")

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

static bool ExtractBoolField(const std::string& content, const std::string& fieldName, bool defaultValue = false)
{
    size_t pos = content.find("\"" + fieldName + "\"");
    if (pos == std::string::npos) return defaultValue;

    pos = content.find(":", pos);
    if (pos == std::string::npos) return defaultValue;

    size_t valPos = content.find_first_not_of(" \t\r\n", pos + 1);
    if (valPos == std::string::npos) return defaultValue;

    if (content.compare(valPos, 4, "true") == 0) return true;
    if (content.compare(valPos, 5, "false") == 0) return false;
    return defaultValue;
}

static double ExtractDoubleField(const std::string& content, const std::string& fieldName, double defaultValue)
{
    size_t pos = content.find("\"" + fieldName + "\"");
    if (pos == std::string::npos) return defaultValue;

    pos = content.find(":", pos);
    if (pos == std::string::npos) return defaultValue;

    size_t valPos = content.find_first_not_of(" \t\r\n", pos + 1);
    if (valPos == std::string::npos) return defaultValue;

    try {
        return std::stod(content.substr(valPos));
    } catch (...) {
        return defaultValue;
    }
}

static int ExtractIntField(const std::string& content, const std::string& fieldName, int defaultValue)
{
    size_t pos = content.find("\"" + fieldName + "\"");
    if (pos == std::string::npos) return defaultValue;

    pos = content.find(":", pos);
    if (pos == std::string::npos) return defaultValue;

    size_t valPos = content.find_first_not_of(" \t\r\n", pos + 1);
    if (valPos == std::string::npos) return defaultValue;

    try {
        return std::stoi(content.substr(valPos));
    } catch (...) {
        return defaultValue;
    }
}

bool LoadAppConfig(AppConfig* config)
{
    if (config == nullptr) return false;

    config->targetSid = L"";
    config->enabled = false;
    config->matchThreshold = 0.363;
    config->requiredMatches = 3;
    config->windowSize = 5;
    config->scanTimeoutMs = 10000;
    config->livenessEnabled = false;
    config->cameraIndex = 0;

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
    config->matchThreshold = ExtractDoubleField(content, "matchThreshold", 0.363);
    config->requiredMatches = ExtractIntField(content, "requiredMatches", 3);
    config->windowSize = ExtractIntField(content, "windowSize", 5);
    config->scanTimeoutMs = ExtractIntField(content, "scanTimeoutMs", 10000);
    config->livenessEnabled = ExtractBoolField(content, "livenessEnabled", false);
    config->cameraIndex = ExtractIntField(content, "cameraIndex", 0);

    if (config->matchThreshold <= 0.0 || config->matchThreshold > 1.0) config->matchThreshold = 0.363;
    if (config->windowSize < 1) config->windowSize = 5;
    if (config->windowSize > 30) config->windowSize = 30;
    if (config->requiredMatches < 1) config->requiredMatches = 3;
    if (config->requiredMatches > config->windowSize) config->requiredMatches = config->windowSize;
    if (config->scanTimeoutMs < 1000) config->scanTimeoutMs = 10000;
    if (config->scanTimeoutMs > 30000) config->scanTimeoutMs = 30000;
    if (config->cameraIndex < 0) config->cameraIndex = 0;

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

    if (header.protectedBlobSize > 65536)
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

// ─── PIN Verification ───────────────────────────────────────────────────────
//
// pin.bin format:
//   PinFileHeader (28 bytes total):
//     magic          (4B)  = 'HFLP' (0x504C4648)
//     schemaVersion  (2B)  = 1
//     flags          (2B)  = 0
//     iterations     (4B)  = PBKDF2 iteration count
//     salt           (16B) = random salt
//   Followed by:
//     hash           (32B) = PBKDF2-SHA256 derived key

struct PinFileHeader {
    uint32_t magic;          // 'HFLP' (0x504C4648)
    uint16_t schemaVersion;  // 1
    uint16_t flags;          // 0
    uint32_t iterations;     // PBKDF2 iterations
    uint8_t  salt[16];       // Random salt
};

const uint32_t HFLP_MAGIC = 0x504C4648;
const uint32_t PIN_HASH_SIZE = 32;

bool VerifyPin(PCWSTR pwzPin)
{
    if (pwzPin == nullptr || pwzPin[0] == L'\0') return false;

    // Load pin.bin
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        return false;
    }
    PathAppendW(path, L"HomeFaceLogon");
    PathAppendW(path, L"pin.bin");

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    PinFileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (file.gcount() != sizeof(header) || header.magic != HFLP_MAGIC || header.schemaVersion != 1)
    {
        file.close();
        return false;
    }

    uint8_t storedHash[PIN_HASH_SIZE];
    file.read(reinterpret_cast<char*>(storedHash), PIN_HASH_SIZE);
    if (file.gcount() != PIN_HASH_SIZE)
    {
        file.close();
        return false;
    }
    file.close();

    // Convert PIN to UTF-8 for hashing
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pwzPin, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return false;

    std::vector<char> pinUtf8(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, pwzPin, -1, pinUtf8.data(), utf8Len, nullptr, nullptr);
    // Don't include the null terminator in the password length
    ULONG pinLen = static_cast<ULONG>(utf8Len - 1);

    // Derive key using PBKDF2-SHA256 via Windows BCrypt API
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status))
    {
        SecureZeroMemory(pinUtf8.data(), pinUtf8.size());
        return false;
    }

    uint8_t derivedKey[PIN_HASH_SIZE];
    status = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(pinUtf8.data()),
        pinLen,
        const_cast<PUCHAR>(header.salt),
        sizeof(header.salt),
        header.iterations,
        derivedKey,
        PIN_HASH_SIZE,
        0
    );

    BCryptCloseAlgorithmProvider(hAlg, 0);
    SecureZeroMemory(pinUtf8.data(), pinUtf8.size());

    if (!BCRYPT_SUCCESS(status))
    {
        return false;
    }

    // Constant-time comparison to prevent timing attacks
    ULONG diff = 0;
    for (ULONG i = 0; i < PIN_HASH_SIZE; ++i)
    {
        diff |= derivedKey[i] ^ storedHash[i];
    }

    SecureZeroMemory(derivedKey, sizeof(derivedKey));

    return (diff == 0);
}
