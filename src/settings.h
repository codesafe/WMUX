#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

struct Settings {
    std::wstring fontName = L"Consolas";
    float fontSize = 14.0f;
    int windowWidth = 1200;
    int windowHeight = 800;
    bool dimInactivePanes = true;  // Dim inactive panes by default
    uint32_t backgroundColor = 0x1E1E1E;  // RGB background color (default: dark gray)
    uint32_t separatorColor = 0x404040;  // RGB separator color
    int prefixTimeoutMs = 1500;
    int scrollLines = 0;  // 0 = use system setting
    int idleScrambleMinutes = 5;  // 0 = disabled
    bool showPrefixOverlay = true;

    void Load();
    void Save() const;

    static std::wstring GetConfigPath();
};
