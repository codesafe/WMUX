#pragma once
#include "pty/conpty.h"
#include "terminal/buffer.h"
#include "terminal/parser.h"
#include <string>
#include <queue>
#include <mutex>

class Pane {
public:
    Pane();

    bool Start(int cols, int rows, HWND hwnd, UINT msg,
               const std::wstring& shell = L"", uint32_t paneId = 0,
               const std::wstring& workingDir = L"");
    void Stop();
    void ProcessOutput();
    void SendInput(const char* data, size_t len);
    void SendInput(const std::string& data);
    void Resize(int cols, int rows);
    void RefreshWorkingDirectory();

    TerminalBuffer& GetBuffer() { return m_buffer; }
    const TerminalBuffer& GetBuffer() const { return m_buffer; }
    ConPty& GetPty() { return m_pty; }
    bool IsRunning() const { return m_pty.IsRunning(); }
    bool IsReady() const { return m_pty.IsReady(); }
    std::wstring GetWorkingDirectory() const { return m_pty.GetWorkingDirectory(); }
    void TryFlushPendingInput();

private:
    void FlushPendingInput();

    ConPty m_pty;
    TerminalBuffer m_buffer;
    VtParser m_parser;

    std::mutex m_pendingInputMutex;
    std::queue<std::string> m_pendingInput;
};
