// config.h
#pragma once
#include <string>

struct AppConfig {
    std::wstring targetSid;
    bool enabled;
};

bool LoadAppConfig(AppConfig* config);
