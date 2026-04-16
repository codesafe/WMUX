#include "settings.h"
#include <cstdio>

std::wstring Settings::GetConfigPath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    // Remove filename, keep directory
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    return std::wstring(exePath) + L"config.ini";
}

void Settings::Load() {
    std::wstring path = GetConfigPath();
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    // Detect and delete old-format config (no INI sections)
    {
        wchar_t probe[8] = {};
        GetPrivateProfileStringW(L"font", L"name", L"", probe, 8, path.c_str());
        if (probe[0] == L'\0') {
            // Section [font] not found - old format file, delete it
            DeleteFileW(path.c_str());
            return;
        }
    }

    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"font", L"name", L"Consolas",
                             buf, 256, path.c_str());
    fontName = buf;
    // Trim whitespace
    while (!fontName.empty() && fontName.back() <= L' ') fontName.pop_back();
    while (!fontName.empty() && fontName.front() <= L' ') fontName.erase(fontName.begin());
    if (fontName.empty()) fontName = L"Consolas";

    fontSize = static_cast<float>(
        GetPrivateProfileIntW(L"font", L"size_x10", 140, path.c_str())) / 10.0f;
    if (fontSize < 6.0f) fontSize = 6.0f;
    if (fontSize > 72.0f) fontSize = 72.0f;

    windowWidth = GetPrivateProfileIntW(L"window", L"width", 1200, path.c_str());
    if (windowWidth < 200) windowWidth = 200;

    windowHeight = GetPrivateProfileIntW(L"window", L"height", 800, path.c_str());
    if (windowHeight < 150) windowHeight = 150;

    dimInactivePanes = GetPrivateProfileIntW(L"appearance", L"dim_inactive_panes", 1, path.c_str()) != 0;

    // Background color as hex (e.g., "1E1E1E")
    wchar_t colorBuf[16] = {};
    GetPrivateProfileStringW(L"appearance", L"background_color", L"1E1E1E",
                             colorBuf, 16, path.c_str());
    backgroundColor = static_cast<uint32_t>(wcstoul(colorBuf, nullptr, 16));
}

void Settings::Save() const {
    std::wstring path = GetConfigPath();

    // Delete old file to avoid format conflicts
    DeleteFileW(path.c_str());

    WritePrivateProfileStringW(L"font", L"name",
                               fontName.c_str(), path.c_str());

    wchar_t buf[32];
    swprintf_s(buf, L"%d", static_cast<int>(fontSize * 10.0f + 0.5f));
    WritePrivateProfileStringW(L"font", L"size_x10", buf, path.c_str());

    swprintf_s(buf, L"%d", windowWidth);
    WritePrivateProfileStringW(L"window", L"width", buf, path.c_str());

    swprintf_s(buf, L"%d", windowHeight);
    WritePrivateProfileStringW(L"window", L"height", buf, path.c_str());

    swprintf_s(buf, L"%d", dimInactivePanes ? 1 : 0);
    WritePrivateProfileStringW(L"appearance", L"dim_inactive_panes", buf, path.c_str());

    swprintf_s(buf, L"%06X", backgroundColor);
    WritePrivateProfileStringW(L"appearance", L"background_color", buf, path.c_str());
}
