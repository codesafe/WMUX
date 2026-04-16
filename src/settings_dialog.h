#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <functional>
#include "settings.h"

using SettingsPreviewCallback = std::function<void(const Settings&)>;

// Shows modal settings dialog. Returns true if user pressed OK.
bool ShowSettingsDialog(HWND parent, Settings& settings,
                        const SettingsPreviewCallback& onPreview = {});
