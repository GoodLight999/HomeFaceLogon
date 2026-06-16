// config.h
#pragma once
#include <windows.h>
#include <string>

struct AppConfig {
    std::wstring targetSid;
    bool enabled;
};

bool LoadAppConfig(AppConfig* config);
bool LoadAndDecryptPassword(PWSTR* ppszPassword);
