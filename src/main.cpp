#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include "app.h"

#pragma comment(lib, "ole32.lib")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR /*lpCmdLine*/, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    App app;
    int result = 1;

    if (app.Initialize(hInstance, nCmdShow))
        result = app.Run();

    CoUninitialize();
    return result;
}
