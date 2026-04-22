#include "pane/session_host.h"
#include "pane/pane.h"
#include "pane/session_protocol.h"
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <vector>

namespace {

void AppendRuntimeLog(const std::wstring& message) {
    wchar_t tempPath[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempPath);
    if (len == 0 || len >= MAX_PATH)
        return;

    std::wstring path = tempPath;
    path += L"wmux_runtime.log";

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wstringstream line;
    line << L"[host " << GetCurrentProcessId() << L"] "
         << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"."
         << st.wMilliseconds << L" " << message << L"\r\n";
    std::wstring text = line.str();

    DWORD written = 0;
    WriteFile(file, text.data(),
              static_cast<DWORD>(text.size() * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(file);
}

int CountVisibleCells(const TerminalBufferSnapshot& snapshot) {
    int count = 0;
    for (const Cell& cell : snapshot.cells) {
        if (cell.width > 0 && cell.ch != 0 && cell.ch != L' ')
            count++;
    }
    return count;
}

std::wstring FirstRowPreview(const TerminalBufferSnapshot& snapshot, int maxChars) {
    std::wstring text;
    if (snapshot.rows <= 0 || snapshot.cols <= 0 || maxChars <= 0)
        return text;

    int cols = (std::min)(snapshot.cols, maxChars);
    text.reserve(cols);
    for (int c = 0; c < cols; c++) {
        const Cell& cell = snapshot.cells[c];
        if (cell.width == 0)
            continue;
        text += (cell.ch == 0) ? L' ' : cell.ch;
    }
    return text;
}

class SessionHostWindow {
public:
    SessionHostWindow(const std::wstring& sessionId, int cols, int rows,
                      const std::wstring& shell, const std::wstring& workingDir)
        : m_pipeName(SessionProtocol::BuildPipeName(sessionId)),
          m_cols(cols),
          m_rows(rows),
          m_shell(shell),
          m_workingDir(workingDir) {}

    int Run(HINSTANCE hInstance) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &SessionHostWindow::WndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = L"WmuxSessionHostWindow";
        RegisterClassW(&wc);

        m_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0,
                                 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                 hInstance, this);
        if (!m_hwnd)
            return 1;

        if (!m_pane.Start(m_cols, m_rows, m_hwnd, WM_APP + 1, m_shell, 1, m_workingDir))
            return 1;
        AppendRuntimeLog(L"SessionHostWindow pane started");

        m_running = true;
        m_serverThread = std::thread(&SessionHostWindow::PipeServerThread, this);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        m_running = false;
        if (m_serverThread.joinable()) {
            CancelSynchronousIo(m_serverThread.native_handle());
            m_serverThread.join();
        }
        m_pane.Stop();
        return static_cast<int>(msg.wParam);
    }

private:
    static constexpr UINT WM_SEND_SNAPSHOT = WM_APP + 2;
    static constexpr UINT WM_START_RECONNECT_TIMER = WM_APP + 3;
    static constexpr UINT WM_STOP_RECONNECT_TIMER = WM_APP + 4;
    static constexpr UINT WM_PIPE_INPUT = WM_APP + 5;
    static constexpr UINT WM_PIPE_RESIZE = WM_APP + 6;
    static constexpr UINT TIMER_RECONNECT = 2;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        SessionHostWindow* self = nullptr;
        if (msg == WM_CREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<SessionHostWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<SessionHostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!self)
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        if (msg == WM_APP + 1) {
            if (wParam == 1) {
                self->SendExit();
                PostQuitMessage(0);
                return 0;
            }
            self->m_pane.ProcessOutput();
            if (!self->m_snapshotScheduled) {
                self->m_snapshotScheduled = true;
                PostMessageW(hwnd, WM_SEND_SNAPSHOT, 0, 0);
            }
            return 0;
        }

        if (msg == WM_SEND_SNAPSHOT) {
            self->m_snapshotScheduled = false;
            self->SendSnapshot();
            return 0;
        }

        if (msg == WM_PIPE_INPUT) {
            std::string data;
            {
                std::lock_guard<std::mutex> lock(self->m_pendingInputMutex);
                if (!self->m_pendingInputQueue.empty()) {
                    data = std::move(self->m_pendingInputQueue.front());
                    self->m_pendingInputQueue.pop();
                }
            }
            if (!data.empty())
                self->m_pane.SendInput(data);
            return 0;
        }

        if (msg == WM_PIPE_RESIZE) {
            int cols = LOWORD(wParam);
            int rows = HIWORD(wParam);
            if (cols > 0 && rows > 0)
                self->m_pane.Resize(cols, rows);
            return 0;
        }

        if (msg == WM_START_RECONNECT_TIMER) {
            SetTimer(hwnd, TIMER_RECONNECT, 30000, nullptr);
            return 0;
        }

        if (msg == WM_STOP_RECONNECT_TIMER) {
            KillTimer(hwnd, TIMER_RECONNECT);
            return 0;
        }

        if (msg == WM_TIMER && wParam == TIMER_RECONNECT) {
            KillTimer(hwnd, TIMER_RECONNECT);
            if (self->m_waitingForClient.load()) {
                self->m_running = false;
                CancelSynchronousIo(self->m_serverThread.native_handle());
                PostQuitMessage(0);
            }
            return 0;
        }

        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void PipeServerThread() {
        bool hadClient = false;
        while (m_running) {
            HANDLE pipe = CreateNamedPipeW(
                m_pipeName.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                1,
                1 << 20,
                1 << 20,
                0,
                nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
                return;

            if (hadClient)
                PostMessageW(m_hwnd, WM_START_RECONNECT_TIMER, 0, 0);

            m_waitingForClient = true;
            BOOL connected = ConnectNamedPipe(pipe, nullptr);
            m_waitingForClient = false;

            if (hadClient)
                PostMessageW(m_hwnd, WM_STOP_RECONNECT_TIMER, 0, 0);

            if (!connected) {
                DWORD err = GetLastError();
                if (err != ERROR_PIPE_CONNECTED) {
                    CloseHandle(pipe);
                    if (!m_running) return;
                    continue;
                }
            }
            hadClient = true;

            {
                std::lock_guard<std::mutex> lock(m_pipeMutex);
                if (m_clientPipe != INVALID_HANDLE_VALUE)
                    CloseHandle(m_clientPipe);
                m_clientPipe = pipe;
            }

            auto initialSnapshot = m_pane.GetBuffer().CreateSnapshot();
            if (CountVisibleCells(initialSnapshot) > 0 ||
                !initialSnapshot.title.empty() ||
                !initialSnapshot.scrollback.empty()) {
                SendSnapshot();
            } else {
                AppendRuntimeLog(L"Skip initial blank snapshot");
            }

            while (m_running) {
                DWORD avail = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr))
                    break;
                if (avail == 0) {
                    Sleep(5);
                    continue;
                }

                SessionProtocol::MessageType type{};
                std::vector<uint8_t> payload;
                if (!SessionProtocol::ReadMessage(pipe, type, payload))
                    break;

                if (type == SessionProtocol::MessageType::Input) {
                    std::string data;
                    if (SessionProtocol::ParseInputPayload(payload, data)) {
                        {
                            std::lock_guard<std::mutex> lock(m_pendingInputMutex);
                            m_pendingInputQueue.push(std::move(data));
                        }
                        PostMessageW(m_hwnd, WM_PIPE_INPUT, 0, 0);
                    }
                } else if (type == SessionProtocol::MessageType::Resize) {
                    int cols = 0, rows = 0;
                    if (SessionProtocol::ParseResizePayload(payload, cols, rows))
                        PostMessageW(m_hwnd, WM_PIPE_RESIZE, MAKEWPARAM(cols, rows), 0);
                } else if (type == SessionProtocol::MessageType::Shutdown) {
                    m_running = false;
                    PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
                    break;
                }
            }

            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            std::lock_guard<std::mutex> lock(m_pipeMutex);
            if (m_clientPipe == pipe)
                m_clientPipe = INVALID_HANDLE_VALUE;
        }
    }

    void SendSnapshot() {
        std::lock_guard<std::mutex> lock(m_pipeMutex);
        if (m_clientPipe == INVALID_HANDLE_VALUE)
            return;
        auto snapshot = m_pane.GetBuffer().CreateSnapshot();
        {
            std::wstringstream ss;
            ss << L"SendSnapshot cols=" << snapshot.cols
               << L" rows=" << snapshot.rows
               << L" visibleCells=" << CountVisibleCells(snapshot)
               << L" titleBytes=" << snapshot.title.size()
               << L" running=" << (m_pane.IsRunning() ? 1 : 0)
               << L" ready=" << (m_pane.IsReady() ? 1 : 0)
               << L" row0=\"" << FirstRowPreview(snapshot, 48) << L"\"";
            AppendRuntimeLog(ss.str());
        }
        auto payload = SessionProtocol::BuildSnapshotPayload(
            snapshot, m_pane.GetWorkingDirectory(), m_pane.IsRunning(), m_pane.IsReady());
        SessionProtocol::WriteMessage(m_clientPipe, SessionProtocol::MessageType::Snapshot, payload);
    }

    void SendExit() {
        std::lock_guard<std::mutex> lock(m_pipeMutex);
        if (m_clientPipe == INVALID_HANDLE_VALUE)
            return;
        auto payload = SessionProtocol::BuildExitedPayload(0);
        SessionProtocol::WriteMessage(m_clientPipe, SessionProtocol::MessageType::Exited, payload);
    }

    HWND m_hwnd = nullptr;
    Pane m_pane;
    std::wstring m_pipeName;
    int m_cols = 0;
    int m_rows = 0;
    std::wstring m_shell;
    std::wstring m_workingDir;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_waitingForClient{false};
    std::thread m_serverThread;
    std::mutex m_pipeMutex;
    HANDLE m_clientPipe = INVALID_HANDLE_VALUE;
    bool m_snapshotScheduled = false;
    std::mutex m_pendingInputMutex;
    std::queue<std::string> m_pendingInputQueue;
};

}

int RunPaneSessionHost(HINSTANCE hInstance, const std::wstring& sessionId,
                       int cols, int rows,
                       const std::wstring& shell,
                       const std::wstring& workingDir) {
    SessionHostWindow window(sessionId, cols, rows, shell, workingDir);
    return window.Run(hInstance);
}
