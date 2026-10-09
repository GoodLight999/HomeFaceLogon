#pragma once
#include <windows.h>
#include <istream>
#include <iostream>
#include <string>

// The manager sends line-delimited UTF-8, independently of the machine locale.
inline bool ReadUtf8CredentialLine(std::istream& input, std::wstring& value)
{
    std::string bytes;
    if (!std::getline(input, bytes)) return false;
    if (!bytes.empty() && bytes.back() == '\r') bytes.pop_back();
    const auto clear = [&]() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); };
    if (bytes.find('\0') != std::string::npos) { clear(); return false; }
    if (bytes.empty()) { value.clear(); return true; }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) { clear(); return false; }
    value.resize(length);
    bool ok = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), value.data(), length) == length;
    clear();
    return ok;
}

inline bool ReadCredentialLine(std::wstring& value)
{
    DWORD mode = 0;
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode))
        return static_cast<bool>(std::getline(std::wcin, value));
    return ReadUtf8CredentialLine(std::cin, value);
}
