// config.h
#pragma once
#include <windows.h>
#include <string>

struct AppConfig {
    std::wstring targetSid;
    bool enabled;
    double matchThreshold;     // Cosine similarity threshold for face match
    int requiredMatches;       // Number of matching frames required
    int windowSize;            // Sliding window size for match tracking
    int scanTimeoutMs;         // Camera scan timeout in milliseconds
    bool livenessEnabled;      // Whether motion-based liveness check is enabled
    int cameraIndex;           // Camera device index
};

bool LoadAppConfig(AppConfig* config);
bool LoadAndDecryptPassword(PWSTR* ppszPassword);

// PIN verification using PBKDF2-SHA256
// Returns true if the provided PIN matches the stored hash in pin.bin
bool VerifyPin(PCWSTR pwzPin);
