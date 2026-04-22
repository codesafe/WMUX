#include "pane/session_protocol.h"
#include <cstring>

namespace SessionProtocol {

namespace {

template <typename T>
void AppendPod(std::vector<uint8_t>& out, const T& value) {
    size_t oldSize = out.size();
    out.resize(oldSize + sizeof(T));
    memcpy(out.data() + oldSize, &value, sizeof(T));
}

template <typename T>
bool ReadPod(const std::vector<uint8_t>& in, size_t& offset, T& value) {
    if (offset + sizeof(T) > in.size())
        return false;
    memcpy(&value, in.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t len) {
    size_t oldSize = out.size();
    out.resize(oldSize + len);
    memcpy(out.data() + oldSize, data, len);
}

bool ReadBytes(const std::vector<uint8_t>& in, size_t& offset, void* data, size_t len) {
    if (offset + len > in.size())
        return false;
    memcpy(data, in.data() + offset, len);
    offset += len;
    return true;
}

void AppendString(std::vector<uint8_t>& out, const std::wstring& text) {
    uint32_t chars = static_cast<uint32_t>(text.size());
    AppendPod(out, chars);
    if (!text.empty())
        AppendBytes(out, text.data(), text.size() * sizeof(wchar_t));
}

bool ReadString(const std::vector<uint8_t>& in, size_t& offset, std::wstring& text) {
    uint32_t chars = 0;
    if (!ReadPod(in, offset, chars))
        return false;
    size_t needed = static_cast<size_t>(chars) * sizeof(wchar_t);
    if (offset + needed > in.size())
        return false;
    text.resize(chars);
    if (chars == 0)
        return true;
    return ReadBytes(in, offset, text.data(), needed);
}

void AppendString8(std::vector<uint8_t>& out, const std::string& text) {
    uint32_t bytes = static_cast<uint32_t>(text.size());
    AppendPod(out, bytes);
    if (!text.empty())
        AppendBytes(out, text.data(), text.size());
}

bool ReadString8(const std::vector<uint8_t>& in, size_t& offset, std::string& text) {
    uint32_t bytes = 0;
    if (!ReadPod(in, offset, bytes))
        return false;
    if (offset + bytes > in.size())
        return false;
    text.resize(bytes);
    if (bytes == 0)
        return true;
    return ReadBytes(in, offset, text.data(), bytes);
}

void AppendCells(std::vector<uint8_t>& out, const std::vector<Cell>& cells) {
    uint32_t count = static_cast<uint32_t>(cells.size());
    AppendPod(out, count);
    if (!cells.empty())
        AppendBytes(out, cells.data(), cells.size() * sizeof(Cell));
}

bool ReadCells(const std::vector<uint8_t>& in, size_t& offset, std::vector<Cell>& cells) {
    uint32_t count = 0;
    if (!ReadPod(in, offset, count))
        return false;
    size_t needed = static_cast<size_t>(count) * sizeof(Cell);
    if (offset + needed > in.size())
        return false;
    cells.resize(count);
    if (count == 0)
        return true;
    return ReadBytes(in, offset, cells.data(), needed);
}

void AppendScrollback(std::vector<uint8_t>& out, const std::deque<std::vector<Cell>>& lines) {
    uint32_t lineCount = static_cast<uint32_t>(lines.size());
    AppendPod(out, lineCount);
    for (const auto& line : lines)
        AppendCells(out, line);
}

bool ReadScrollback(const std::vector<uint8_t>& in, size_t& offset, std::deque<std::vector<Cell>>& lines) {
    uint32_t lineCount = 0;
    if (!ReadPod(in, offset, lineCount))
        return false;
    lines.clear();
    for (uint32_t i = 0; i < lineCount; i++) {
        std::vector<Cell> line;
        if (!ReadCells(in, offset, line))
            return false;
        lines.push_back(std::move(line));
    }
    return true;
}

}

bool WriteMessage(HANDLE pipe, MessageType type, const std::vector<uint8_t>& payload) {
    MessageHeader header{};
    header.type = static_cast<uint32_t>(type);
    header.size = static_cast<uint32_t>(payload.size());

    std::vector<uint8_t> buf(sizeof(header) + payload.size());
    memcpy(buf.data(), &header, sizeof(header));
    if (!payload.empty())
        memcpy(buf.data() + sizeof(header), payload.data(), payload.size());

    DWORD written = 0;
    return WriteFile(pipe, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == buf.size();
}

bool ReadMessage(HANDLE pipe, MessageType& type, std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf(sizeof(MessageHeader));
    DWORD read = 0;
    BOOL ok = ReadFile(pipe, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);

    if (!ok && GetLastError() == ERROR_MORE_DATA) {
        DWORD bytesLeft = 0;
        PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, &bytesLeft);
        size_t headerRead = read;
        buf.resize(headerRead + bytesLeft);
        DWORD read2 = 0;
        if (!ReadFile(pipe, buf.data() + headerRead, bytesLeft, &read2, nullptr))
            return false;
        read = static_cast<DWORD>(headerRead + read2);
    } else if (!ok || read < sizeof(MessageHeader)) {
        return false;
    }

    MessageHeader header{};
    memcpy(&header, buf.data(), sizeof(header));
    type = static_cast<MessageType>(header.type);

    payload.clear();
    if (read > sizeof(MessageHeader))
        payload.assign(buf.data() + sizeof(MessageHeader), buf.data() + read);
    return true;
}

std::vector<uint8_t> BuildHelloPayload(int cols, int rows,
                                       const std::wstring& shell,
                                       const std::wstring& workingDir) {
    std::vector<uint8_t> payload;
    HelloPayload hello{};
    hello.cols = static_cast<uint32_t>(cols);
    hello.rows = static_cast<uint32_t>(rows);
    hello.shellChars = static_cast<uint32_t>(shell.size());
    hello.cwdChars = static_cast<uint32_t>(workingDir.size());
    AppendPod(payload, hello);
    if (!shell.empty())
        AppendBytes(payload, shell.data(), shell.size() * sizeof(wchar_t));
    if (!workingDir.empty())
        AppendBytes(payload, workingDir.data(), workingDir.size() * sizeof(wchar_t));
    return payload;
}

bool ParseHelloPayload(const std::vector<uint8_t>& payload, int& cols, int& rows,
                       std::wstring& shell, std::wstring& workingDir) {
    size_t offset = 0;
    HelloPayload hello{};
    if (!ReadPod(payload, offset, hello))
        return false;
    cols = static_cast<int>(hello.cols);
    rows = static_cast<int>(hello.rows);
    shell.resize(hello.shellChars);
    workingDir.resize(hello.cwdChars);
    if (hello.shellChars > 0 &&
        !ReadBytes(payload, offset, shell.data(), shell.size() * sizeof(wchar_t))) {
        return false;
    }
    if (hello.cwdChars > 0 &&
        !ReadBytes(payload, offset, workingDir.data(), workingDir.size() * sizeof(wchar_t))) {
        return false;
    }
    return true;
}

std::vector<uint8_t> BuildInputPayload(const char* data, size_t len) {
    std::vector<uint8_t> payload;
    uint32_t bytes = static_cast<uint32_t>(len);
    AppendPod(payload, bytes);
    if (len > 0)
        AppendBytes(payload, data, len);
    return payload;
}

bool ParseInputPayload(const std::vector<uint8_t>& payload, std::string& data) {
    size_t offset = 0;
    uint32_t bytes = 0;
    if (!ReadPod(payload, offset, bytes))
        return false;
    data.resize(bytes);
    if (bytes == 0)
        return true;
    return ReadBytes(payload, offset, data.data(), bytes);
}

std::vector<uint8_t> BuildResizePayload(int cols, int rows) {
    std::vector<uint8_t> payload;
    ResizePayload resize{};
    resize.cols = static_cast<uint32_t>(cols);
    resize.rows = static_cast<uint32_t>(rows);
    AppendPod(payload, resize);
    return payload;
}

bool ParseResizePayload(const std::vector<uint8_t>& payload, int& cols, int& rows) {
    size_t offset = 0;
    ResizePayload resize{};
    if (!ReadPod(payload, offset, resize))
        return false;
    cols = static_cast<int>(resize.cols);
    rows = static_cast<int>(resize.rows);
    return true;
}

std::vector<uint8_t> BuildExitedPayload(uint32_t exitCode) {
    std::vector<uint8_t> payload;
    ExitedPayload exited{};
    exited.exitCode = exitCode;
    AppendPod(payload, exited);
    return payload;
}

bool ParseExitedPayload(const std::vector<uint8_t>& payload, uint32_t& exitCode) {
    size_t offset = 0;
    ExitedPayload exited{};
    if (!ReadPod(payload, offset, exited))
        return false;
    exitCode = exited.exitCode;
    return true;
}

std::vector<uint8_t> BuildSnapshotPayload(const TerminalBufferSnapshot& snapshot,
                                          const std::wstring& workingDir,
                                          bool running, bool ready) {
    std::vector<uint8_t> payload;
    AppendCells(payload, snapshot.cells);
    AppendPod(payload, snapshot.cols);
    AppendPod(payload, snapshot.rows);
    AppendPod(payload, snapshot.cursorRow);
    AppendPod(payload, snapshot.cursorCol);
    AppendPod(payload, snapshot.wrapPending);
    AppendPod(payload, snapshot.attr);
    AppendPod(payload, snapshot.scrollTop);
    AppendPod(payload, snapshot.scrollBottom);
    AppendPod(payload, snapshot.cursorVisible);
    AppendPod(payload, snapshot.appCursorKeys);
    AppendPod(payload, snapshot.bracketedPaste);
    AppendString8(payload, snapshot.title);
    AppendPod(payload, snapshot.savedCursorRow);
    AppendPod(payload, snapshot.savedCursorCol);
    AppendPod(payload, snapshot.savedAttr);
    AppendScrollback(payload, snapshot.scrollback);
    AppendPod(payload, snapshot.scrollOffset);
    AppendPod(payload, snapshot.maxScrollback);
    AppendPod(payload, snapshot.altScreenActive);
    AppendCells(payload, snapshot.savedMainBuffer);
    AppendPod(payload, snapshot.savedMainCursorRow);
    AppendPod(payload, snapshot.savedMainCursorCol);
    AppendPod(payload, snapshot.savedMainAttr);
    AppendString(payload, workingDir);
    AppendPod(payload, running);
    AppendPod(payload, ready);
    return payload;
}

bool ParseSnapshotPayload(const std::vector<uint8_t>& payload,
                          TerminalBufferSnapshot& snapshot,
                          std::wstring& workingDir,
                          bool& running, bool& ready) {
    size_t offset = 0;
    if (!ReadCells(payload, offset, snapshot.cells)) return false;
    if (!ReadPod(payload, offset, snapshot.cols)) return false;
    if (!ReadPod(payload, offset, snapshot.rows)) return false;
    if (!ReadPod(payload, offset, snapshot.cursorRow)) return false;
    if (!ReadPod(payload, offset, snapshot.cursorCol)) return false;
    if (!ReadPod(payload, offset, snapshot.wrapPending)) return false;
    if (!ReadPod(payload, offset, snapshot.attr)) return false;
    if (!ReadPod(payload, offset, snapshot.scrollTop)) return false;
    if (!ReadPod(payload, offset, snapshot.scrollBottom)) return false;
    if (!ReadPod(payload, offset, snapshot.cursorVisible)) return false;
    if (!ReadPod(payload, offset, snapshot.appCursorKeys)) return false;
    if (!ReadPod(payload, offset, snapshot.bracketedPaste)) return false;
    if (!ReadString8(payload, offset, snapshot.title)) return false;
    if (!ReadPod(payload, offset, snapshot.savedCursorRow)) return false;
    if (!ReadPod(payload, offset, snapshot.savedCursorCol)) return false;
    if (!ReadPod(payload, offset, snapshot.savedAttr)) return false;
    if (!ReadScrollback(payload, offset, snapshot.scrollback)) return false;
    if (!ReadPod(payload, offset, snapshot.scrollOffset)) return false;
    if (!ReadPod(payload, offset, snapshot.maxScrollback)) return false;
    if (!ReadPod(payload, offset, snapshot.altScreenActive)) return false;
    if (!ReadCells(payload, offset, snapshot.savedMainBuffer)) return false;
    if (!ReadPod(payload, offset, snapshot.savedMainCursorRow)) return false;
    if (!ReadPod(payload, offset, snapshot.savedMainCursorCol)) return false;
    if (!ReadPod(payload, offset, snapshot.savedMainAttr)) return false;
    if (!ReadString(payload, offset, workingDir)) return false;
    if (!ReadPod(payload, offset, running)) return false;
    if (!ReadPod(payload, offset, ready)) return false;
    return true;
}

std::wstring BuildPipeName(const std::wstring& sessionId) {
    return L"\\\\.\\pipe\\wmux-session-" + sessionId;
}

}
