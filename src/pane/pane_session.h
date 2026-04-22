#pragma once
#include "terminal/buffer.h"
#include <Windows.h>
#include <cstdint>
#include <string>

struct PaneDescriptor {
    uint32_t cols = 0;
    uint32_t rows = 0;
    std::wstring workingDirectory;
    std::string title;
    std::wstring sessionToken;
    bool externallyDetachable = false;
};

class IPaneSession {
public:
    virtual ~IPaneSession() = default;

    virtual bool Start(int cols, int rows, HWND hwnd, UINT msg,
                       const std::wstring& shell = L"", uint32_t paneId = 0,
                       const std::wstring& workingDir = L"") = 0;
    virtual void Stop() = 0;
    virtual void ProcessOutput() = 0;
    virtual void SendInput(const char* data, size_t len) = 0;
    virtual void SendInput(const std::string& data) = 0;
    virtual void Resize(int cols, int rows) = 0;
    virtual void RefreshWorkingDirectory() = 0;
    virtual TerminalBuffer& GetBuffer() = 0;
    virtual const TerminalBuffer& GetBuffer() const = 0;
    virtual bool IsRunning() const = 0;
    virtual bool IsReady() const = 0;
    virtual std::wstring GetWorkingDirectory() const = 0;
    virtual void TryFlushPendingInput() = 0;
    virtual PaneDescriptor Describe() const = 0;
    virtual std::wstring GetSessionToken() const = 0;
    virtual void PrepareForMove() = 0;
};
