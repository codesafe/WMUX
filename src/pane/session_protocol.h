#pragma once
#include "terminal/buffer.h"
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace SessionProtocol {

enum class MessageType : uint32_t {
    Snapshot = 1,
    Exited = 2,
    Hello = 100,
    Input = 101,
    Resize = 102,
    Shutdown = 103,
};

struct MessageHeader {
    uint32_t type = 0;
    uint32_t size = 0;
};

struct HelloPayload {
    uint32_t cols = 0;
    uint32_t rows = 0;
    uint32_t shellChars = 0;
    uint32_t cwdChars = 0;
};

struct ResizePayload {
    uint32_t cols = 0;
    uint32_t rows = 0;
};

struct ExitedPayload {
    uint32_t exitCode = 0;
};

bool WriteMessage(HANDLE pipe, MessageType type, const std::vector<uint8_t>& payload);
bool ReadMessage(HANDLE pipe, MessageType& type, std::vector<uint8_t>& payload);

std::vector<uint8_t> BuildHelloPayload(int cols, int rows,
                                       const std::wstring& shell,
                                       const std::wstring& workingDir);
bool ParseHelloPayload(const std::vector<uint8_t>& payload, int& cols, int& rows,
                       std::wstring& shell, std::wstring& workingDir);

std::vector<uint8_t> BuildInputPayload(const char* data, size_t len);
bool ParseInputPayload(const std::vector<uint8_t>& payload, std::string& data);

std::vector<uint8_t> BuildResizePayload(int cols, int rows);
bool ParseResizePayload(const std::vector<uint8_t>& payload, int& cols, int& rows);

std::vector<uint8_t> BuildExitedPayload(uint32_t exitCode);
bool ParseExitedPayload(const std::vector<uint8_t>& payload, uint32_t& exitCode);

std::vector<uint8_t> BuildSnapshotPayload(const TerminalBufferSnapshot& snapshot,
                                          const std::wstring& workingDir,
                                          bool running, bool ready);
bool ParseSnapshotPayload(const std::vector<uint8_t>& payload,
                          TerminalBufferSnapshot& snapshot,
                          std::wstring& workingDir,
                          bool& running, bool& ready);

std::wstring BuildPipeName(const std::wstring& sessionId);

}
