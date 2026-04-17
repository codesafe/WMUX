#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include "app.h"

#pragma comment(lib, "ole32.lib")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR /*lpCmdLine*/, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\wmux_single_instance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        int choice = MessageBoxW(
            nullptr,
            L"WMUX가 이미 실행 중입니다.\n"
            L"기존 WMUX에 새 창으로 추가하시겠습니까?\n",
            //L"[\uc544\ub2c8\uc624] \uc0c8 \ucc3d\uc73c\ub85c \uc2e4\ud589\n"
            //L"[\ucde8\uc18c] \uc2e4\ud589 \uc548 \ud568",
            L"wmux",
            MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON1);

        if (choice == IDYES) {
            HWND existing = FindWindowW(L"WmuxWindowClass", nullptr);
            if (existing) {
                wchar_t cwd[MAX_PATH] = {};
                std::wstring workingDir;
                DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwd);
                if (cwdLen > 0)
                    workingDir.assign(cwd, cwdLen);

                COPYDATASTRUCT copyData = {};
                copyData.dwData = App::GetAddPaneCopyDataId();
                copyData.cbData = static_cast<DWORD>((workingDir.size() + 1) * sizeof(wchar_t));
                copyData.lpData = workingDir.empty() ? nullptr : workingDir.data();
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copyData),
                                    SMTO_ABORTIFHUNG, 2000, &ignored);
            }

            CloseHandle(instanceMutex);
            CoUninitialize();
            return 0;
        }

        if (choice == IDCANCEL) {
            CloseHandle(instanceMutex);
            CoUninitialize();
            return 0;
        }
    }

    App app;
    int result = 1;

    if (app.Initialize(hInstance, nCmdShow))
        result = app.Run();

    CoUninitialize();
    if (instanceMutex)
        CloseHandle(instanceMutex);
    return result;
}
