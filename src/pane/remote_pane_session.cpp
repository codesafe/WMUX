#include "pane/remote_pane_session.h"
#include <objbase.h>
#include <sstream>

namespace {
std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring quoted = L"\"";
    for (wchar_t ch : arg) {
        if (ch == L'"')
            quoted += L'\\';
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

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
    line << L"[ui " << GetCurrentProcessId() << L"] "
         << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"."
         << st.wMilliseconds << L" " << message << L"\r\n";
    std::wstring text = line.str();

    DWORD written = 0;
    WriteFile(file, text.data(),
              static_cast<DWORD>(text.size() * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(file);
}
}

RemotePaneSession::RemotePaneSession(const std::wstring& existingSessionId, bool ownsHost)
    : m_sessionId(existingSessionId), m_ownsHost(ownsHost) {
    m_buffer.Init(1, 1);
    if (!m_sessionId.empty())
        m_pipeName = SessionProtocol::BuildPipeName(m_sessionId);
}

RemotePaneSession::~RemotePaneSession() {
    Stop();
}

bool RemotePaneSession::Start(int cols, int rows, HWND hwnd, UINT msg,
                              const std::wstring& shell, uint32_t paneId,
                              const std::wstring& workingDir) {
    {
        std::wstringstream ss;
        ss << L"Start paneId=" << paneId
           << L" cols=" << cols
           << L" rows=" << rows
           << L" ownsHost=" << (m_ownsHost ? 1 : 0);
        AppendRuntimeLog(ss.str());
    }
    m_notifyHwnd = hwnd;
    m_notifyMsg = msg;
    m_notifyLParam = static_cast<LPARAM>(paneId);
    m_workingDirectory = workingDir;
    m_lastCols = cols;
    m_lastRows = rows;
    bool attachingToExisting = !m_sessionId.empty();
    if (m_sessionId.empty()) {
        m_sessionId = GenerateSessionId();
        m_pipeName = SessionProtocol::BuildPipeName(m_sessionId);
    }

    if (m_ownsHost && !attachingToExisting) {
        if (!LaunchHostProcess(cols, rows, shell, workingDir)) {
            AppendRuntimeLog(L"LaunchHostProcess failed");
            return false;
        }
    }

    if (!ConnectToHost()) {
        AppendRuntimeLog(L"ConnectToHost failed");
        Stop();
        return false;
    }

    m_running = true;
    if (!ReceiveInitialSnapshot())
        AppendRuntimeLog(L"ReceiveInitialSnapshot skipped or timed out");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastCols = m_buffer.GetCols();
        m_lastRows = m_buffer.GetRows();
    }

    m_readerThread = std::thread(&RemotePaneSession::ReaderThread, this);
    m_writerThread = std::thread(&RemotePaneSession::WriterThread, this);
    return true;
}

void RemotePaneSession::Stop() {
    m_running = false;
    m_outboundCv.notify_all();

    if (m_writerThread.joinable())
        m_writerThread.join();

    if (m_ownsHost && m_pipe != INVALID_HANDLE_VALUE) {
        SessionProtocol::WriteMessage(m_pipe, SessionProtocol::MessageType::Shutdown, {});
        FlushFileBuffers(m_pipe);
    }

    if (m_readerThread.joinable())
        m_readerThread.join();

    if (m_ownsHost && m_hostProcess.hProcess) {
        if (WaitForSingleObject(m_hostProcess.hProcess, 3000) == WAIT_TIMEOUT)
            TerminateProcess(m_hostProcess.hProcess, 1);
    } else if (m_ownsHost && m_pipe != INVALID_HANDLE_VALUE) {
        Sleep(100);
    }

    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    if (m_hostProcess.hProcess) {
        CloseHandle(m_hostProcess.hProcess);
        m_hostProcess.hProcess = nullptr;
    }
    if (m_hostProcess.hThread) {
        CloseHandle(m_hostProcess.hThread);
        m_hostProcess.hThread = nullptr;
    }
}

void RemotePaneSession::ProcessOutput() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasPendingSnapshot)
        return;

    if (m_resizePending && m_pendingSnapshot.altScreenActive) {
        int currentVisible = 0;
        int rows = m_buffer.GetRows();
        int cols = m_buffer.GetCols();
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                const Cell& cell = m_buffer.At(r, c);
                if (cell.width > 0 && cell.ch != 0 && cell.ch != L' ')
                    currentVisible++;
            }
        }

        int newVisible = 0;
        for (const Cell& cell : m_pendingSnapshot.cells) {
            if (cell.width > 0 && cell.ch != 0 && cell.ch != L' ')
                newVisible++;
        }

        if (currentVisible > 0 && newVisible * 2 < currentVisible) {
            std::wstringstream ss;
            ss << L"ProcessOutput: skip degraded alt-screen snapshot after resize"
               << L" cur=" << currentVisible << L" new=" << newVisible;
            AppendRuntimeLog(ss.str());
            m_hasPendingSnapshot = false;
            m_updatePosted = false;
            return;
        }
    }
    m_resizePending = false;

    {
        std::wstringstream ss;
        ss << L"ProcessOutput cols=" << m_pendingSnapshot.cols
           << L" rows=" << m_pendingSnapshot.rows
           << L" ready=" << (m_pendingReady ? 1 : 0)
           << L" running=" << (m_pendingRunning ? 1 : 0)
           << L" titleBytes=" << m_pendingSnapshot.title.size();
        AppendRuntimeLog(ss.str());
    }
    m_buffer.LoadSnapshot(m_pendingSnapshot);
    m_workingDirectory = m_pendingWorkingDirectory;
    m_ready = m_pendingReady;
    m_hasPendingSnapshot = false;
    m_updatePosted = false;
}

void RemotePaneSession::SendInput(const char* data, size_t len) {
    SendMessage(SessionProtocol::MessageType::Input,
                SessionProtocol::BuildInputPayload(data, len));
}

void RemotePaneSession::SendInput(const std::string& data) {
    SendInput(data.c_str(), data.size());
}

void RemotePaneSession::Resize(int cols, int rows) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (cols > 0 && rows > 0 && (m_lastCols != cols || m_lastRows != rows)) {
            m_lastCols = cols;
            m_lastRows = rows;
            m_buffer.Resize(cols, rows);
            m_resizePending = true;
            changed = true;
        }
    }
    if (!changed)
        return;
    SendMessage(SessionProtocol::MessageType::Resize,
                SessionProtocol::BuildResizePayload(cols, rows));
}

void RemotePaneSession::RefreshWorkingDirectory() {
}

std::wstring RemotePaneSession::GetWorkingDirectory() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_workingDirectory;
}

PaneDescriptor RemotePaneSession::Describe() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    PaneDescriptor desc;
    desc.cols = static_cast<uint32_t>(m_buffer.GetCols());
    desc.rows = static_cast<uint32_t>(m_buffer.GetRows());
    desc.workingDirectory = m_workingDirectory;
    desc.title = m_buffer.GetTitle();
    desc.sessionToken = m_sessionId;
    desc.externallyDetachable = true;
    return desc;
}

bool RemotePaneSession::ReceiveInitialSnapshot() {
    if (m_pipe == INVALID_HANDLE_VALUE)
        return false;

    DWORD deadline = GetTickCount() + 2000;
    while (GetTickCount() < deadline) {
        DWORD bytesAvailable = 0;
        if (PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr) && bytesAvailable > 0) {
            SessionProtocol::MessageType type{};
            std::vector<uint8_t> payload;
            if (!SessionProtocol::ReadMessage(m_pipe, type, payload))
                return false;

            if (type == SessionProtocol::MessageType::Snapshot) {
                TerminalBufferSnapshot snapshot;
                std::wstring workingDir;
                bool running = false;
                bool ready = false;
                if (!SessionProtocol::ParseSnapshotPayload(payload, snapshot, workingDir, running, ready))
                    return false;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_buffer.LoadSnapshot(snapshot);
                    m_workingDirectory = std::move(workingDir);
                    m_ready = ready;
                    m_running = running;
                    m_hasPendingSnapshot = false;
                    m_updatePosted = false;
                }

                std::wstringstream ss;
                ss << L"ReceiveInitialSnapshot cols=" << snapshot.cols
                   << L" rows=" << snapshot.rows
                   << L" ready=" << (ready ? 1 : 0)
                   << L" running=" << (running ? 1 : 0)
                   << L"titleBytes=" << snapshot.title.size();
                AppendRuntimeLog(ss.str());
                return true;
            }

            if (type == SessionProtocol::MessageType::Exited) {
                m_running = false;
                m_ready = false;
                return false;
            }
        }

        Sleep(10);
    }

    return false;
}

bool RemotePaneSession::ConnectToHost() {
    constexpr DWORD kConnectTimeoutMs = 5000;
    DWORD deadline = GetTickCount() + kConnectTimeoutMs;

    while (GetTickCount() < deadline) {
        HANDLE pipe = CreateFileW(m_pipeName.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            m_pipe = pipe;
            AppendRuntimeLog(L"ConnectToHost success");
            return true;
        }

        if (GetLastError() != ERROR_PIPE_BUSY)
            Sleep(50);
        else
            WaitNamedPipeW(m_pipeName.c_str(), 200);
    }

    return false;
}

bool RemotePaneSession::LaunchHostProcess(int cols, int rows, const std::wstring& shell,
                                          const std::wstring& workingDir) {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return false;

    std::wstringstream cmd;
    cmd << QuoteArg(exePath)
        << L" --session-host " << QuoteArg(m_sessionId)
        << L" --cols " << cols
        << L" --rows " << rows;
    if (!shell.empty())
        cmd << L" --shell " << QuoteArg(shell);
    if (!workingDir.empty())
        cmd << L" --cwd " << QuoteArg(workingDir);

    std::wstring command = cmd.str();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    ZeroMemory(&m_hostProcess, sizeof(m_hostProcess));
    return CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                          nullptr, nullptr, &si, &m_hostProcess) == TRUE;
}

void RemotePaneSession::ReaderThread() {
    while (m_running && m_pipe != INVALID_HANDLE_VALUE) {
        DWORD avail = 0;
        if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &avail, nullptr))
            break;
        if (avail == 0) {
            Sleep(5);
            continue;
        }

        SessionProtocol::MessageType type{};
        std::vector<uint8_t> payload;
        if (!SessionProtocol::ReadMessage(m_pipe, type, payload))
            break;

        if (type == SessionProtocol::MessageType::Snapshot) {
            TerminalBufferSnapshot snapshot;
            std::wstring workingDir;
            bool running = false;
            bool ready = false;
            if (SessionProtocol::ParseSnapshotPayload(payload, snapshot, workingDir, running, ready)) {
                {
                    std::wstringstream ss;
                    ss << L"ReaderThread snapshot cols=" << snapshot.cols
                       << L" rows=" << snapshot.rows
                       << L" ready=" << (ready ? 1 : 0)
                       << L" running=" << (running ? 1 : 0)
                       << L" titleBytes=" << snapshot.title.size();
                    AppendRuntimeLog(ss.str());
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_pendingSnapshot = std::move(snapshot);
                    m_pendingWorkingDirectory = std::move(workingDir);
                    m_pendingRunning = running;
                    m_pendingReady = ready;
                    m_hasPendingSnapshot = true;
                    if (!m_updatePosted) {
                        m_updatePosted = true;
                        if (m_notifyHwnd)
                            PostMessage(m_notifyHwnd, m_notifyMsg, 0, m_notifyLParam);
                    }
                }
            }
        } else if (type == SessionProtocol::MessageType::Exited) {
            m_running = false;
            m_ready = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_notifyHwnd)
                    PostMessage(m_notifyHwnd, m_notifyMsg, 1, m_notifyLParam);
            }
            break;
        }
    }

    m_running = false;
    m_outboundCv.notify_all();
}

void RemotePaneSession::WriterThread() {
    while (true) {
        OutboundMessage msg;
        {
            std::unique_lock<std::mutex> lock(m_outboundMutex);
            m_outboundCv.wait(lock, [&] {
                return !m_outboundQueue.empty() || !m_running.load();
            });

            if (m_outboundQueue.empty()) {
                if (!m_running.load())
                    break;
                continue;
            }

            msg = std::move(m_outboundQueue.front());
            m_outboundQueue.pop();
        }

        HANDLE pipe = m_pipe;
        if (pipe == INVALID_HANDLE_VALUE)
            continue;

        if (!SessionProtocol::WriteMessage(pipe, msg.type, msg.payload))
            break;
    }
}

bool RemotePaneSession::SendMessage(SessionProtocol::MessageType type,
                                    const std::vector<uint8_t>& payload) {
    if (m_pipe == INVALID_HANDLE_VALUE || !m_running.load())
        return false;
    {
        std::lock_guard<std::mutex> lock(m_outboundMutex);
        m_outboundQueue.push(OutboundMessage{type, payload});
    }
    m_outboundCv.notify_one();
    return true;
}

std::wstring RemotePaneSession::GenerateSessionId() {
    GUID guid{};
    if (CoCreateGuid(&guid) != S_OK)
        return L"fallback";

    wchar_t buf[64] = {};
    StringFromGUID2(guid, buf, static_cast<int>(std::size(buf)));
    std::wstring id = buf;
    for (wchar_t& ch : id) {
        if (ch == L'{' || ch == L'}' || ch == L'-')
            ch = L'_';
    }
    return id;
}
