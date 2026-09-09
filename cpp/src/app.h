#pragma once

#include <windows.h>

#include <string>

// Entry point for the native hotspot launcher. Returns the process exit code.
int appMain(HINSTANCE instance, const std::wstring& commandLine, int showCmd);
