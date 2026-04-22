#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <shellapi.h>
#include "app.h"
#include "pane/session_host.h"
#include <string>

#pragma comment(lib, "ole32.lib")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR /*lpCmdLine*/, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        std::wstring sessionId;
        std::wstring attachSessionId;
        std::wstring shell;
        std::wstring workingDir;
        int cols = 80;
        int rows = 25;
        int attachZone = 4;
        bool forceNewWindow = false;

        for (int i = 1; i < argc; i++) {
            std::wstring arg = argv[i];
            if (arg == L"--session-host" && i + 1 < argc) {
                sessionId = argv[++i];
            } else if (arg == L"--attach-session" && i + 1 < argc) {
                attachSessionId = argv[++i];
            } else if (arg == L"--zone" && i + 1 < argc) {
                attachZone = _wtoi(argv[++i]);
            } else if (arg == L"--cols" && i + 1 < argc) {
                cols = _wtoi(argv[++i]);
            } else if (arg == L"--rows" && i + 1 < argc) {
                rows = _wtoi(argv[++i]);
            } else if (arg == L"--shell" && i + 1 < argc) {
                shell = argv[++i];
            } else if (arg == L"--cwd" && i + 1 < argc) {
                workingDir = argv[++i];
            } else if (arg == L"--new-window") {
                forceNewWindow = true;
            }
        }

        LocalFree(argv);

        if (!sessionId.empty()) {
            int result = RunPaneSessionHost(hInstance, sessionId, cols, rows, shell, workingDir);
            CoUninitialize();
            return result;
        }
        if (!attachSessionId.empty() && !forceNewWindow) {
            HWND existing = FindWindowW(L"WmuxWindowClass", nullptr);
            if (existing) {
                std::wstring payload = std::to_wstring(attachZone) + L"\n" + attachSessionId;
                COPYDATASTRUCT copyData = {};
                copyData.dwData = App::GetAttachPaneCopyDataId();
                copyData.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
                copyData.lpData = payload.data();
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copyData),
                                    SMTO_ABORTIFHUNG, 2000, &ignored);
                CoUninitialize();
                return 0;
            }
        }

        App app;
        int result = 1;

        if (app.Initialize(hInstance, nCmdShow, attachSessionId, workingDir))
            result = app.Run();

        CoUninitialize();
        return result;
    }
    CoUninitialize();
    return 1;
}
