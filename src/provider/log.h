// log.h
#pragma once
#include <windows.h>
#include <string>

void LogInit();
void LogShutdown();
void LogInfo(const wchar_t* format, ...);
void LogError(const wchar_t* format, ...);

std::wstring MaskQualifiedUserName(const std::wstring& name);
std::wstring MaskSid(const std::wstring& sid);
