#include "pane/pane.h"

static void PaneWriteCallback(const char* data, size_t len, void* ctx) {
    auto* pane = static_cast<Pane*>(ctx);
    pane->GetPty().Write(data, len);
}

Pane::Pane() : m_parser(m_buffer) {
    m_parser.SetWriteCallback(PaneWriteCallback, this);
}

bool Pane::Start(int cols, int rows, HWND hwnd, UINT msg,
                 const std::wstring& shell, uint32_t paneId,
                 const std::wstring& workingDir) {
    m_buffer.Init(cols, rows);
    return m_pty.Start(cols, rows, hwnd, msg, shell, static_cast<LPARAM>(paneId),
                       workingDir);
}

void Pane::Stop() {
    m_pty.Stop();
}

void Pane::ProcessOutput() {
    std::string data = m_pty.ConsumeOutput();
    if (!data.empty()) {
        m_parser.ProcessBytes(data.c_str(), data.size());
    }
}

void Pane::SendInput(const char* data, size_t len) {
    m_pty.Write(data, len);
}

void Pane::SendInput(const std::string& data) {
    m_pty.Write(data.c_str(), data.size());
}

void Pane::Resize(int cols, int rows) {
    m_buffer.Resize(cols, rows);
    m_pty.Resize(cols, rows);
}
