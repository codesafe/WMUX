#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "settings.h"

// Shows modal settings dialog. Returns true if user pressed OK.
bool ShowSettingsDialog(HWND parent, Settings& settings);
