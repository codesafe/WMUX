#pragma once
#include <Windows.h>
#include <string>

int RunPaneSessionHost(HINSTANCE hInstance, const std::wstring& sessionId,
                       int cols, int rows,
                       const std::wstring& shell,
                       const std::wstring& workingDir);
