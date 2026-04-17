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
    showPrefixOverlay = GetPrivateProfileIntW(L"appearance", L"show_prefix_overlay", 1, path.c_str()) != 0;

    // Background color as hex (e.g., "1E1E1E")
    wchar_t colorBuf[16] = {};
    GetPrivateProfileStringW(L"appearance", L"background_color", L"1E1E1E",
                             colorBuf, 16, path.c_str());
    backgroundColor = static_cast<uint32_t>(wcstoul(colorBuf, nullptr, 16));

    GetPrivateProfileStringW(L"appearance", L"separator_color", L"404040",
                             colorBuf, 16, path.c_str());
    separatorColor = static_cast<uint32_t>(wcstoul(colorBuf, nullptr, 16));

    prefixTimeoutMs = GetPrivateProfileIntW(L"input", L"prefix_timeout_ms", 1500, path.c_str());
    if (prefixTimeoutMs < 250) prefixTimeoutMs = 250;
    if (prefixTimeoutMs > 10000) prefixTimeoutMs = 10000;

    scrollLines = GetPrivateProfileIntW(L"input", L"scroll_lines", 0, path.c_str());
    if (scrollLines < 0) scrollLines = 0;
    if (scrollLines > 100) scrollLines = 100;

    idleScrambleMinutes = GetPrivateProfileIntW(L"input", L"idle_scramble_minutes", 5, path.c_str());
    if (idleScrambleMinutes < 0) idleScrambleMinutes = 0;
    if (idleScrambleMinutes > 240) idleScrambleMinutes = 240;
}

void Settings::Save() const {
    std::wstring path = GetConfigPath();

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

    swprintf_s(buf, L"%06X", separatorColor);
    WritePrivateProfileStringW(L"appearance", L"separator_color", buf, path.c_str());

    swprintf_s(buf, L"%d", showPrefixOverlay ? 1 : 0);
    WritePrivateProfileStringW(L"appearance", L"show_prefix_overlay", buf, path.c_str());

    swprintf_s(buf, L"%d", prefixTimeoutMs);
    WritePrivateProfileStringW(L"input", L"prefix_timeout_ms", buf, path.c_str());

    swprintf_s(buf, L"%d", scrollLines);
    WritePrivateProfileStringW(L"input", L"scroll_lines", buf, path.c_str());

    swprintf_s(buf, L"%d", idleScrambleMinutes);
    WritePrivateProfileStringW(L"input", L"idle_scramble_minutes", buf, path.c_str());
}
