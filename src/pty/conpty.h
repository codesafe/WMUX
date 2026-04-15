#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

class ConPty {
public:
    ConPty() = default;
    ~ConPty();

    ConPty(const ConPty&) = delete;
    ConPty& operator=(const ConPty&) = delete;

    bool Start(int cols, int rows, HWND notifyHwnd, UINT notifyMsg,
               const std::wstring& command = L"", LPARAM notifyLParam = 0);
    void Stop();
    void Write(const char* data, size_t len);
    void Resize(int cols, int rows);

    std::string ConsumeOutput();
    bool IsRunning() const { return m_running.load(); }

private:
    void ReaderThread();
    static void CALLBACK ProcessExitCallback(PVOID context, BOOLEAN timedOut);

    HPCON m_hPC = nullptr;
    HANDLE m_processWaitHandle = nullptr;
    HANDLE m_pipeWrite = INVALID_HANDLE_VALUE;
    HANDLE m_pipeRead = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION m_pi = {};

    std::thread m_readerThread;
    std::atomic<bool> m_running{false};

    std::mutex m_outputMutex;
    std::string m_outputBuffer;

    HWND m_notifyHwnd = nullptr;
    UINT m_notifyMsg = 0;
    LPARAM m_notifyLParam = 0;
};
