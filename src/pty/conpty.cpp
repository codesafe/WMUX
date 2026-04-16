#include "pty/conpty.h"
#include <vector>

ConPty::~ConPty() {
    Stop();
}

bool ConPty::Start(int cols, int rows, HWND notifyHwnd, UINT notifyMsg,
                   const std::wstring& command, LPARAM notifyLParam,
                   const std::wstring& workingDir) {
    m_notifyHwnd = notifyHwnd;
    m_notifyMsg = notifyMsg;
    m_notifyLParam = notifyLParam;

    // Use provided working directory or current directory
    if (!workingDir.empty()) {
        m_workingDirectory = workingDir;
    } else {
        wchar_t cwd[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
            m_workingDirectory = cwd;
        }
    }

    HANDLE pipeInRead = INVALID_HANDLE_VALUE;
    HANDLE pipeOutWrite = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&pipeInRead, &m_pipeWrite, nullptr, 0))
        return false;
    if (!CreatePipe(&m_pipeRead, &pipeOutWrite, nullptr, 0)) {
        CloseHandle(pipeInRead);
        CloseHandle(m_pipeWrite);
        m_pipeWrite = INVALID_HANDLE_VALUE;
        return false;
    }

    COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    HRESULT hr = CreatePseudoConsole(size, pipeInRead, pipeOutWrite, 0, &m_hPC);
    CloseHandle(pipeInRead);
    CloseHandle(pipeOutWrite);

    if (FAILED(hr)) {
        CloseHandle(m_pipeWrite);
        CloseHandle(m_pipeRead);
        m_pipeWrite = INVALID_HANDLE_VALUE;
        m_pipeRead = INVALID_HANDLE_VALUE;
        return false;
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    auto attrList = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!attrList) {
        ClosePseudoConsole(m_hPC);
        m_hPC = nullptr;
        return false;
    }

    InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              m_hPC, sizeof(m_hPC), nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrList;

    std::wstring cmd;
    if (command.empty()) {
        wchar_t comspec[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"COMSPEC", comspec, MAX_PATH) > 0)
            cmd = comspec;
        else
            cmd = L"C:\\Windows\\System32\\cmd.exe";
    } else {
        cmd = command;
    }

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    // Use working directory if specified
    const wchar_t* lpCurrentDirectory = m_workingDirectory.empty()
        ? nullptr : m_workingDirectory.c_str();

    BOOL success = CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT, nullptr, lpCurrentDirectory,
        &si.StartupInfo, &m_pi);

    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);

    if (!success) {
        ClosePseudoConsole(m_hPC);
        m_hPC = nullptr;
        CloseHandle(m_pipeWrite);
        CloseHandle(m_pipeRead);
        m_pipeWrite = INVALID_HANDLE_VALUE;
        m_pipeRead = INVALID_HANDLE_VALUE;
        return false;
    }

    m_running = true;
    m_readerThread = std::thread(&ConPty::ReaderThread, this);

    // Monitor child process exit: close pseudo console to unblock ReadFile
    RegisterWaitForSingleObject(&m_processWaitHandle, m_pi.hProcess,
        ProcessExitCallback, this, INFINITE, WT_EXECUTEONLYONCE);

    return true;
}

void CALLBACK ConPty::ProcessExitCallback(PVOID context, BOOLEAN /*timedOut*/) {
    auto* self = static_cast<ConPty*>(context);
    // Close pseudo console from threadpool - unblocks ReaderThread's ReadFile
    HPCON hpc = self->m_hPC;
    self->m_hPC = nullptr;
    if (hpc)
        ClosePseudoConsole(hpc);
}

void ConPty::Stop() {
    m_running = false;

    // Unregister process wait before closing handles
    if (m_processWaitHandle) {
        UnregisterWaitEx(m_processWaitHandle, INVALID_HANDLE_VALUE);
        m_processWaitHandle = nullptr;
    }

    if (m_hPC) {
        ClosePseudoConsole(m_hPC);
        m_hPC = nullptr;
    }

    if (m_readerThread.joinable())
        m_readerThread.join();

    if (m_pipeWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipeWrite);
        m_pipeWrite = INVALID_HANDLE_VALUE;
    }
    if (m_pipeRead != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipeRead);
        m_pipeRead = INVALID_HANDLE_VALUE;
    }
    if (m_pi.hProcess) {
        CloseHandle(m_pi.hProcess);
        m_pi.hProcess = nullptr;
    }
    if (m_pi.hThread) {
        CloseHandle(m_pi.hThread);
        m_pi.hThread = nullptr;
    }
}

void ConPty::Write(const char* data, size_t len) {
    if (m_pipeWrite == INVALID_HANDLE_VALUE)
        return;
    DWORD written;
    WriteFile(m_pipeWrite, data, static_cast<DWORD>(len), &written, nullptr);
}

void ConPty::Resize(int cols, int rows) {
    if (m_hPC) {
        COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
        ResizePseudoConsole(m_hPC, size);
    }
}

std::string ConPty::ConsumeOutput() {
    std::lock_guard<std::mutex> lock(m_outputMutex);
    std::string data = std::move(m_outputBuffer);
    m_outputBuffer.clear();
    return data;
}

void ConPty::ReaderThread() {
    char buf[4096];
    DWORD bytesRead;
    while (m_running) {
        BOOL ok = ReadFile(m_pipeRead, buf, sizeof(buf), &bytesRead, nullptr);
        if (ok && bytesRead > 0) {
            {
                std::lock_guard<std::mutex> lock(m_outputMutex);
                m_outputBuffer.append(buf, bytesRead);
            }
            if (m_notifyHwnd)
                PostMessage(m_notifyHwnd, m_notifyMsg, 0, m_notifyLParam);
        } else {
            break;
        }
    }
    m_running = false;
    if (m_notifyHwnd)
        PostMessage(m_notifyHwnd, m_notifyMsg, 1, m_notifyLParam);
}
