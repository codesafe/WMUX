#include "pane/pane.h"
#include <cwctype>

namespace {
std::wstring TrimSpaces(const std::wstring& text) {
    size_t start = 0;
    while (start < text.size() && iswspace(text[start]))
        start++;

    size_t end = text.size();
    while (end > start && iswspace(text[end - 1]))
        end--;

    return text.substr(start, end - start);
}

bool LooksLikeWindowsPath(const std::wstring& path) {
    if (path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }

    return path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\';
}

bool TryExtractPromptPath(const std::wstring& line, std::wstring& outPath) {
    std::wstring trimmed = TrimSpaces(line);
    if (trimmed.empty() || trimmed.back() != L'>')
        return false;

    size_t psPos = trimmed.rfind(L"PS ");
    if (psPos != std::wstring::npos) {
        std::wstring candidate = TrimSpaces(trimmed.substr(psPos + 3, trimmed.size() - psPos - 4));
        if (LooksLikeWindowsPath(candidate)) {
            outPath = std::move(candidate);
            return true;
        }
    }

    std::wstring candidate = TrimSpaces(trimmed.substr(0, trimmed.size() - 1));
    if (LooksLikeWindowsPath(candidate)) {
        outPath = std::move(candidate);
        return true;
    }

    return false;
}

std::wstring BufferRowToString(const TerminalBuffer& buffer, int row, int maxCols) {
    std::wstring text;
    if (row < 0 || row >= buffer.GetRows() || maxCols <= 0)
        return text;

    int cols = (std::min)(buffer.GetCols(), maxCols);
    text.reserve(cols);
    for (int c = 0; c < cols; c++) {
        const Cell& cell = buffer.At(row, c);
        if (cell.width == 0)
            continue;
        text += (cell.ch == 0) ? L' ' : cell.ch;
        if (cell.ch2 != 0)
            text += cell.ch2;
    }
    return text;
}
}

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
        RefreshWorkingDirectory();

        // Flush pending input when pane becomes ready
        if (m_pty.IsReady()) {
            FlushPendingInput();
        }
    }
}

void Pane::SendInput(const char* data, size_t len) {
    if (!m_pty.IsReady()) {
        std::lock_guard<std::mutex> lock(m_pendingInputMutex);
        m_pendingInput.push(std::string(data, len));
        return;
    }
    m_pty.Write(data, len);
}

void Pane::SendInput(const std::string& data) {
    if (!m_pty.IsReady()) {
        std::lock_guard<std::mutex> lock(m_pendingInputMutex);
        m_pendingInput.push(data);
        return;
    }
    m_pty.Write(data.c_str(), data.size());
}

void Pane::FlushPendingInput() {
    std::lock_guard<std::mutex> lock(m_pendingInputMutex);
    if (m_pendingInput.empty())
        return;

    while (!m_pendingInput.empty()) {
        const std::string& data = m_pendingInput.front();
        m_pty.Write(data.c_str(), data.size());
        m_pendingInput.pop();
    }
}

void Pane::TryFlushPendingInput() {
    if (m_pty.IsReady()) {
        FlushPendingInput();
    }
}

void Pane::Resize(int cols, int rows) {
    m_buffer.Resize(cols, rows);
    m_pty.Resize(cols, rows);
}

void Pane::RefreshWorkingDirectory() {
    std::wstring path;
    int cursorRow = m_buffer.GetCursorRow();
    int cursorCol = m_buffer.GetCursorCol();

    if (TryExtractPromptPath(BufferRowToString(m_buffer, cursorRow, cursorCol), path)) {
        m_pty.SetWorkingDirectory(path);
        return;
    }

    if (cursorCol == 0 && cursorRow > 0 &&
        TryExtractPromptPath(BufferRowToString(m_buffer, cursorRow - 1, m_buffer.GetCols()), path)) {
        m_pty.SetWorkingDirectory(path);
        return;
    }

    m_pty.RefreshWorkingDirectory();
}
