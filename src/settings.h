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

    void Load();
    void Save() const;

    static std::wstring GetConfigPath();
};
