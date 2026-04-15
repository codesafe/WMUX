#pragma once
#include <vector>
#include <cstdint>
#include <string>

class TerminalBuffer;

enum class ParserState {
    Ground,
    Escape,
    EscapeConsume,  // consume next byte after ESC ( / ESC ) / ESC # etc.
    Csi,
    OscString,
    StringPassthrough,  // DCS, APC, PM - ignore until ST (ESC \)
};

class VtParser {
public:
    explicit VtParser(TerminalBuffer& buffer);
    void ProcessBytes(const char* data, size_t len);

    using WriteCallback = void(*)(const char* data, size_t len, void* ctx);
    void SetWriteCallback(WriteCallback cb, void* ctx) {
        m_writeCb = cb; m_writeCtx = ctx;
    }

private:
    void ProcessByte(uint8_t byte);
    void HandleGround(uint8_t byte);
    void HandleControlChar(uint8_t byte);
    void HandleEscape(uint8_t byte);
    void HandleCsi(uint8_t byte);
    void HandleOsc(uint8_t byte);

    void DispatchCsi(uint8_t finalByte);
    void HandleSgr();
    void HandleDecMode(uint8_t action);
    void DispatchOsc();

    void EmitCodepoint(uint32_t cp);

    TerminalBuffer& m_buffer;
    ParserState m_state = ParserState::Ground;

    // CSI parameters
    std::vector<int> m_params;
    bool m_paramStarted = false;
    bool m_privateMarker = false;
    char m_intermediate = 0;

    // OSC
    std::string m_oscString;

    // UTF-8 decoder state
    uint32_t m_utf8Codepoint = 0;
    int m_utf8Remaining = 0;

    // Write callback for terminal responses (DSR, DA)
    WriteCallback m_writeCb = nullptr;
    void* m_writeCtx = nullptr;
    void WriteBack(const char* data, size_t len);
};
