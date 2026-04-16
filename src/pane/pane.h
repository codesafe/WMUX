#pragma once
#include "pty/conpty.h"
#include "terminal/buffer.h"
#include "terminal/parser.h"
#include <string>

class Pane {
public:
    Pane();

    bool Start(int cols, int rows, HWND hwnd, UINT msg,
               const std::wstring& shell = L"", uint32_t paneId = 0);
    void Stop();
    void ProcessOutput();
    void SendInput(const char* data, size_t len);
    void SendInput(const std::string& data);
    void Resize(int cols, int rows);

    TerminalBuffer& GetBuffer() { return m_buffer; }
    const TerminalBuffer& GetBuffer() const { return m_buffer; }
    ConPty& GetPty() { return m_pty; }
    bool IsRunning() const { return m_pty.IsRunning(); }
    std::wstring GetWorkingDirectory() const { return m_pty.GetWorkingDirectory(); }

private:
    ConPty m_pty;
    TerminalBuffer m_buffer;
    VtParser m_parser;
};
