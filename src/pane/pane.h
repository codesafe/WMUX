#pragma once
#include "pane/pane_session.h"
#include "pty/conpty.h"
#include "terminal/parser.h"
#include <string>
#include <queue>
#include <mutex>

class Pane final : public IPaneSession {
public:
    Pane();
    ~Pane() override = default;

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
    ConPty& GetPty() { return m_pty; }
    bool IsRunning() const override { return m_pty.IsRunning(); }
    bool IsReady() const override { return m_pty.IsReady(); }
    std::wstring GetWorkingDirectory() const override { return m_pty.GetWorkingDirectory(); }
    void TryFlushPendingInput() override;
    PaneDescriptor Describe() const override {
        PaneDescriptor desc;
        desc.cols = static_cast<uint32_t>(m_buffer.GetCols());
        desc.rows = static_cast<uint32_t>(m_buffer.GetRows());
        desc.workingDirectory = m_pty.GetWorkingDirectory();
        desc.title = m_buffer.GetTitle();
        desc.externallyDetachable = false;
        return desc;
    }
    std::wstring GetSessionToken() const override { return L""; }
    void PrepareForMove() override {}

private:
    void FlushPendingInput();

    ConPty m_pty;
    TerminalBuffer m_buffer;
    VtParser m_parser;

    std::mutex m_pendingInputMutex;
    std::queue<std::string> m_pendingInput;
};
