#pragma once
#include "pane/pane_session.h"
#include "pane/session_protocol.h"
#include "terminal/buffer.h"
#include <Windows.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

class RemotePaneSession final : public IPaneSession {
public:
    explicit RemotePaneSession(const std::wstring& existingSessionId = L"", bool ownsHost = true);
    ~RemotePaneSession() override;

    bool Start(int cols, int rows, HWND hwnd, UINT msg,
               const std::wstring& shell = L"", uint32_t paneId = 0,
               const std::wstring& workingDir = L"") override;
    void Stop() override;
    void ProcessOutput() override;
    void SendInput(const char* data, size_t len) override;
    void SendInput(const std::string& data) override;
    void Resize(int cols, int rows) override;
    void RefreshWorkingDirectory() override;

    TerminalBuffer& GetBuffer() override { return m_buffer; }
    const TerminalBuffer& GetBuffer() const override { return m_buffer; }
    bool IsRunning() const override { return m_running.load(); }
    bool IsReady() const override { return m_ready.load(); }
    std::wstring GetWorkingDirectory() const override;
    void TryFlushPendingInput() override {}
    PaneDescriptor Describe() const override;
    std::wstring GetSessionToken() const override { return m_sessionId; }
    void PrepareForMove() override {
        m_ownsHost = false;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_notifyHwnd = nullptr;
        m_notifyMsg = 0;
        m_notifyLParam = 0;
    }

private:
    struct OutboundMessage {
        SessionProtocol::MessageType type;
        std::vector<uint8_t> payload;
    };

    bool ReceiveInitialSnapshot();
    bool ConnectToHost();
    bool LaunchHostProcess(int cols, int rows, const std::wstring& shell,
                           const std::wstring& workingDir);
    void ReaderThread();
    void WriterThread();
    bool SendMessage(SessionProtocol::MessageType type, const std::vector<uint8_t>& payload);
    static std::wstring GenerateSessionId();

    TerminalBuffer m_buffer;
    mutable std::mutex m_mutex;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION m_hostProcess = {};
    std::thread m_readerThread;
    std::thread m_writerThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    HWND m_notifyHwnd = nullptr;
    UINT m_notifyMsg = 0;
    LPARAM m_notifyLParam = 0;
    std::wstring m_sessionId;
    std::wstring m_pipeName;
    std::wstring m_workingDirectory;
    bool m_ownsHost = true;
    bool m_hasPendingSnapshot = false;
    TerminalBufferSnapshot m_pendingSnapshot;
    std::wstring m_pendingWorkingDirectory;
    bool m_pendingRunning = false;
    bool m_pendingReady = false;
    bool m_updatePosted = false;
    int m_lastCols = 0;
    int m_lastRows = 0;
    bool m_resizePending = false;
    std::mutex m_outboundMutex;
    std::condition_variable m_outboundCv;
    std::queue<OutboundMessage> m_outboundQueue;
};
