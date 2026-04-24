#include "pane/pane_factory.h"
#include "pane/remote_pane_session.h"
#include "pane/session_protocol.h"
#include <Windows.h>
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

std::wstring GenerateSessionId() {
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
}

std::unique_ptr<IPaneSession> CreatePaneSession() {
    return std::make_unique<RemotePaneSession>();
}

std::unique_ptr<IPaneSession> CreateDetachablePaneSession() {
    return CreatePaneSession();
}

std::unique_ptr<IPaneSession> AttachPaneSession(const std::wstring& sessionId) {
    return std::make_unique<RemotePaneSession>(sessionId, true);
}

std::wstring LaunchDetachedSessionHost(int cols, int rows,
                                       const std::wstring& shell,
                                       const std::wstring& workingDir) {
    std::wstring sessionId = GenerateSessionId();

    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return L"";

    std::wstringstream cmd;
    cmd << QuoteArg(exePath)
        << L" --session-host " << QuoteArg(sessionId)
        << L" --cols " << cols
        << L" --rows " << rows;
    if (!shell.empty())
        cmd << L" --shell " << QuoteArg(shell);
    if (!workingDir.empty())
        cmd << L" --cwd " << QuoteArg(workingDir);

    std::wstring command = cmd.str();
    std::vector<wchar_t> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return L"";
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return sessionId;
}
