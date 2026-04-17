#include "pty/conpty.h"
#include <winternl.h>
#include <vector>
#include <condition_variable>

namespace {
struct CurDirInfo {
    UNICODE_STRING DosPath;
    HANDLE Handle;
};

struct RtlUserProcessParametersPartial {
    ULONG MaximumLength;
    ULONG Length;
    ULONG Flags;
    ULONG DebugFlags;
    HANDLE ConsoleHandle;
    ULONG ConsoleFlags;
    HANDLE StandardInput;
    HANDLE StandardOutput;
    HANDLE StandardError;
    CurDirInfo CurrentDirectory;
};

struct PebPartial {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    PVOID Reserved3[2];
    RtlUserProcessParametersPartial* ProcessParameters;
};

using NtQueryInformationProcessFn =
    NTSTATUS (NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

bool QueryProcessWorkingDirectory(HANDLE process, std::wstring& outPath) {
    if (!process)
        return false;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;

    auto ntQueryInformationProcess = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!ntQueryInformationProcess)
        return false;

    PROCESS_BASIC_INFORMATION pbi = {};
    NTSTATUS status = ntQueryInformationProcess(
        process, ProcessBasicInformation, &pbi, sizeof(pbi), nullptr);
    if (status < 0 || !pbi.PebBaseAddress)
        return false;

    PebPartial peb = {};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process, pbi.PebBaseAddress, &peb, sizeof(peb), &bytesRead) ||
        bytesRead < sizeof(peb) || !peb.ProcessParameters) {
        return false;
    }

    RtlUserProcessParametersPartial params = {};
    bytesRead = 0;
    if (!ReadProcessMemory(process, peb.ProcessParameters, &params, sizeof(params), &bytesRead) ||
        bytesRead < sizeof(params)) {
        return false;
    }

    const UNICODE_STRING& cwd = params.CurrentDirectory.DosPath;
    if (!cwd.Buffer || cwd.Length == 0 || (cwd.Length % sizeof(wchar_t)) != 0)
        return false;

    std::wstring path(cwd.Length / sizeof(wchar_t), L'\0');
    bytesRead = 0;
    if (!ReadProcessMemory(process, cwd.Buffer, path.data(), cwd.Length, &bytesRead) ||
        bytesRead < cwd.Length) {
        return false;
    }

    outPath = std::move(path);
    return true;
}
}

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
    auto cleanupStartup = [&]() {
        if (m_hPC) {
            ClosePseudoConsole(m_hPC);
            m_hPC = nullptr;
        }
        if (m_pipeWrite != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipeWrite);
            m_pipeWrite = INVALID_HANDLE_VALUE;
        }
        if (m_pipeRead != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipeRead);
            m_pipeRead = INVALID_HANDLE_VALUE;
        }
    };

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
        cleanupStartup();
        return false;
    }

    SIZE_T attrSize = 0;
    if (!InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        cleanupStartup();
        return false;
    }
    auto attrList = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!attrList) {
        cleanupStartup();
        return false;
    }

    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   m_hPC, sizeof(m_hPC), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrList);
        HeapFree(GetProcessHeap(), 0, attrList);
        cleanupStartup();
        return false;
    }

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
        cleanupStartup();
        return false;
    }

    m_running = true;
    m_ready = false;
    m_readerThread = std::thread(&ConPty::ReaderThread, this);

    // Monitor child process exit: close pseudo console to unblock ReadFile
    RegisterWaitForSingleObject(&m_processWaitHandle, m_pi.hProcess,
        ProcessExitCallback, this, INFINITE, WT_EXECUTEONLYONCE);

    // Wait for first output (shell prompt) with timeout to ensure PTY is ready
    // Use longer timeout for slower shells like PowerShell (2 seconds)
    {
        std::unique_lock<std::mutex> lock(m_readyMutex);
        m_readyCv.wait_for(lock, std::chrono::milliseconds(2000),
                          [this] { return m_ready.load(); });
    }

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

void ConPty::RefreshWorkingDirectory() {
    std::wstring currentDir;
    if (QueryProcessWorkingDirectory(m_pi.hProcess, currentDir) && !currentDir.empty())
        m_workingDirectory = std::move(currentDir);
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
    bool firstOutput = true;

    while (m_running) {
        BOOL ok = ReadFile(m_pipeRead, buf, sizeof(buf), &bytesRead, nullptr);
        if (ok && bytesRead > 0) {
            {
                std::lock_guard<std::mutex> lock(m_outputMutex);
                m_outputBuffer.append(buf, bytesRead);
            }
            // Signal ready on first output (shell prompt appeared)
            if (firstOutput) {
                firstOutput = false;
                {
                    std::lock_guard<std::mutex> lock(m_readyMutex);
                    m_ready = true;
                }
                m_readyCv.notify_one();
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
