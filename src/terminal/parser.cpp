#include "terminal/parser.h"
#include "terminal/buffer.h"

VtParser::VtParser(TerminalBuffer& buffer) : m_buffer(buffer) {}

void VtParser::ProcessBytes(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++)
        ProcessByte(static_cast<uint8_t>(data[i]));
}

void VtParser::ProcessByte(uint8_t byte) {
    if (m_state == ParserState::StringPassthrough) {
        if (byte == 0x1B)
            m_state = ParserState::StringPassthroughEsc;
        else if (byte == 0x9C) // 8-bit ST
            m_state = ParserState::Ground;
        return;
    }

    if (m_state == ParserState::StringPassthroughEsc) {
        m_state = (byte == '\\') ? ParserState::Ground : ParserState::StringPassthrough;
        return;
    }

    // ESC always resets to Escape state
    if (byte == 0x1B && m_state != ParserState::OscString) {
        m_state = ParserState::Escape;
        m_params.clear();
        m_subParams.clear();
        m_paramStarted = false;
        m_privateMarker = false;
        m_intermediate = 0;
        m_hasColonSubParams = false;
        m_utf8Remaining = 0;
        m_utf8Codepoint = 0;
        return;
    }

    // 8-bit C1 control characters (0x80-0x9F) - only when not in a UTF-8 sequence
    if (byte >= 0x80 && byte <= 0x9F && m_state == ParserState::Ground && m_utf8Remaining == 0) {
        switch (byte) {
        case 0x84: // IND
            m_buffer.Index();
            return;
        case 0x85: // NEL
            m_buffer.NextLine();
            return;
        case 0x88: // HTS
            m_buffer.SetTabStop();
            return;
        case 0x8D: // RI
            m_buffer.ReverseIndex();
            return;
        case 0x90: // DCS
            m_state = ParserState::StringPassthrough;
            return;
        case 0x9B: // CSI
            m_state = ParserState::Csi;
            m_params.clear();
            m_subParams.clear();
            m_paramStarted = false;
            m_privateMarker = false;
            m_intermediate = 0;
            m_hasColonSubParams = false;
            return;
        case 0x9C: // ST
            m_state = ParserState::Ground;
            return;
        case 0x9D: // OSC
            m_state = ParserState::OscString;
            m_oscString.clear();
            return;
        case 0x9E: // PM
        case 0x9F: // APC
            m_state = ParserState::StringPassthrough;
            return;
        }
    }

    // Handle C0 control characters in any state (except some in OSC)
    if (byte < 0x20 && byte != 0x1B) {
        m_utf8Remaining = 0;
        m_utf8Codepoint = 0;
        if (m_state == ParserState::OscString) {
            if (byte == 0x07) { // BEL terminates OSC
                DispatchOsc();
                m_state = ParserState::Ground;
                m_oscString.clear();
                return;
            }
        }
        HandleControlChar(byte);
        return;
    }

    switch (m_state) {
    case ParserState::Ground:
        HandleGround(byte);
        break;
    case ParserState::Escape:
        HandleEscape(byte);
        break;
    case ParserState::EscapeConsume:
        // Consume one byte (charset designator, line attribute, etc.) and return to ground
        m_state = ParserState::Ground;
        break;
    case ParserState::Csi:
        HandleCsi(byte);
        break;
    case ParserState::OscString:
        HandleOsc(byte);
        break;
    case ParserState::StringPassthrough:
    case ParserState::StringPassthroughEsc:
        break;
    }
}

void VtParser::HandleGround(uint8_t byte) {
    if (byte == 0x7F) return; // DEL ignored

    // UTF-8 decoding
    if (m_utf8Remaining > 0) {
        if ((byte & 0xC0) == 0x80) {
            m_utf8Codepoint = (m_utf8Codepoint << 6) | (byte & 0x3F);
            m_utf8Remaining--;
            if (m_utf8Remaining == 0)
                EmitCodepoint(m_utf8Codepoint);
        } else {
            // Invalid continuation, reset and process as new byte
            m_utf8Remaining = 0;
            HandleGround(byte);
        }
        return;
    }

    if (byte < 0x80) {
        EmitCodepoint(byte);
    } else if ((byte & 0xE0) == 0xC0) {
        m_utf8Codepoint = byte & 0x1F;
        m_utf8Remaining = 1;
    } else if ((byte & 0xF0) == 0xE0) {
        m_utf8Codepoint = byte & 0x0F;
        m_utf8Remaining = 2;
    } else if ((byte & 0xF8) == 0xF0) {
        m_utf8Codepoint = byte & 0x07;
        m_utf8Remaining = 3;
    }
}

void VtParser::HandleControlChar(uint8_t byte) {
    switch (byte) {
    case 0x07: break; // BEL
    case 0x08: m_buffer.Backspace(); break;
    case 0x09: m_buffer.Tab(); break;
    case 0x0A: // LF
    case 0x0B: // VT
    case 0x0C: // FF
        m_buffer.LineFeed();
        break;
    case 0x0D: m_buffer.CarriageReturn(); break;
    }
}

void VtParser::HandleEscape(uint8_t byte) {
    switch (byte) {
    case '[':
        m_state = ParserState::Csi;
        m_params.clear();
        m_subParams.clear();
        m_paramStarted = false;
        m_privateMarker = false;
        m_intermediate = 0;
        m_hasColonSubParams = false;
        break;
    case ']':
        m_state = ParserState::OscString;
        m_oscString.clear();
        break;
    case 'D': // IND - Index (move cursor down, scroll if at bottom)
        m_buffer.Index();
        m_state = ParserState::Ground;
        break;
    case 'E': // NEL - Next Line
        m_buffer.NextLine();
        m_state = ParserState::Ground;
        break;
    case 'H': // HTS - Horizontal Tab Set
        m_buffer.SetTabStop();
        m_state = ParserState::Ground;
        break;
    case 'M': // RI - Reverse Index
        m_buffer.ReverseIndex();
        m_state = ParserState::Ground;
        break;
    case '7': // DECSC - Save Cursor
        m_buffer.SaveCursor();
        m_state = ParserState::Ground;
        break;
    case '8': // DECRC - Restore Cursor
        m_buffer.RestoreCursor();
        m_state = ParserState::Ground;
        break;
    case '=': // DECKPAM
    case '>': // DECKPNM
        m_state = ParserState::Ground;
        break;
    case '(': // Set G0 charset
    case ')': // Set G1 charset
    case '*': // Set G2 charset
    case '+': // Set G3 charset
    case '#': // DEC line attributes (DECDHL etc.)
    case ' ': // ESC SP F/G (7/8-bit controls)
        m_state = ParserState::EscapeConsume;
        break;
    case 'P': // DCS - Device Control String
    case '_': // APC - Application Program Command
    case '^': // PM - Privacy Message
        m_state = ParserState::StringPassthrough;
        break;
    case '\\': // ST - String Terminator (ends DCS/OSC/APC/PM)
        m_state = ParserState::Ground;
        break;
    case 'c': // RIS - Full Reset
        m_buffer.FullReset();
        m_state = ParserState::Ground;
        break;
    default:
        m_state = ParserState::Ground;
        break;
    }
}

void VtParser::HandleCsi(uint8_t byte) {
    if (byte == '?' || byte == '>' || byte == '=' || byte == '<') {
        m_privateMarker = true;
        return;
    }

    if (byte == '!') {
        m_intermediate = '!';
        return;
    }

    if (byte >= '0' && byte <= '9') {
        if (!m_paramStarted) {
            m_params.push_back(0);
            m_subParams.push_back({});
            m_paramStarted = true;
        }
        if (m_hasColonSubParams && !m_subParams.empty() && !m_subParams.back().empty()) {
            m_subParams.back().back() = m_subParams.back().back() * 10 + (byte - '0');
        } else {
            m_params.back() = m_params.back() * 10 + (byte - '0');
        }
        return;
    }

    if (byte == ';') {
        if (!m_paramStarted) {
            m_params.push_back(0);
            m_subParams.push_back({});
        }
        m_paramStarted = false;
        m_hasColonSubParams = false;
        return;
    }

    if (byte == ':') {
        if (!m_paramStarted) {
            m_params.push_back(0);
            m_subParams.push_back({});
            m_paramStarted = true;
        }
        m_hasColonSubParams = true;
        m_subParams.back().push_back(0);
        return;
    }

    if (byte >= 0x20 && byte <= 0x2F) {
        m_intermediate = static_cast<char>(byte);
        return;
    }

    if (byte >= 0x40 && byte <= 0x7E) {
        if (!m_paramStarted && !m_params.empty()) {
            // trailing semicolon, add implicit 0
        }
        DispatchCsi(byte);
        m_state = ParserState::Ground;
        return;
    }

    // Unknown byte, abort
    m_state = ParserState::Ground;
}

void VtParser::HandleOsc(uint8_t byte) {
    // OSC terminated by ST (ESC \) or BEL (handled in ProcessByte)
    if (byte == '\\' && !m_oscString.empty() && m_oscString.back() == '\x1B') {
        m_oscString.pop_back();
        DispatchOsc();
        m_state = ParserState::Ground;
        m_oscString.clear();
        return;
    }
    m_oscString += static_cast<char>(byte);
}

void VtParser::DispatchOsc() {
    // OSC format: "code;text"
    auto sep = m_oscString.find(';');
    if (sep == std::string::npos) return;

    int code = 0;
    for (size_t i = 0; i < sep; i++) {
        if (m_oscString[i] >= '0' && m_oscString[i] <= '9')
            code = code * 10 + (m_oscString[i] - '0');
    }

    std::string text = m_oscString.substr(sep + 1);

    switch (code) {
    case 0: // Set icon name and window title
    case 2: // Set window title
        m_buffer.SetTitle(text);
        break;
    case 1: // Set icon name (treat as title)
        m_buffer.SetTitle(text);
        break;
    case 4:   // Set/query color palette - accept silently
    case 8:   // Hyperlinks - accept silently (no rendering support yet)
    case 10:  // Set/query foreground color
    case 11:  // Set/query background color
    case 12:  // Set/query cursor color
    case 52:  // Clipboard operations
    case 133: // Shell integration / semantic zones
        break;
    }
}

void VtParser::DispatchCsi(uint8_t finalByte) {
    if (m_privateMarker) {
        if (m_intermediate == '!' && finalByte == 'p') {
            // CSI ! p - DECSTR (Soft Terminal Reset) - actually not private
            // but some terminals accept CSI ? ! p
        }
        HandleDecMode(finalByte);
        return;
    }

    // CSI ! p - DECSTR (Soft Terminal Reset)
    if (m_intermediate == '!' && finalByte == 'p') {
        m_buffer.SoftReset();
        return;
    }

    auto p = [&](int idx, int def = 1) -> int {
        if (idx < static_cast<int>(m_params.size()) && m_params[idx] > 0)
            return m_params[idx];
        return def;
    };

    switch (finalByte) {
    case 'A': m_buffer.MoveCursorUp(p(0)); break;
    case 'B': m_buffer.MoveCursorDown(p(0)); break;
    case 'C': m_buffer.MoveCursorForward(p(0)); break;
    case 'D': m_buffer.MoveCursorBack(p(0)); break;
    case 'E': // CNL
        m_buffer.MoveCursorDown(p(0));
        m_buffer.CarriageReturn();
        break;
    case 'F': // CPL
        m_buffer.MoveCursorUp(p(0));
        m_buffer.CarriageReturn();
        break;
    case 'G': m_buffer.SetCursorCol(p(0) - 1); break;
    case 'H': case 'f': // CUP
        m_buffer.SetCursorPos(p(0) - 1, p(1) - 1);
        break;
    case 'I': // CHT - Cursor Forward Tab
        m_buffer.TabForward(p(0));
        break;
    case 'J': m_buffer.EraseDisplay(p(0, 0)); break;
    case 'K': m_buffer.EraseLine(p(0, 0)); break;
    case 'L': m_buffer.InsertLines(p(0)); break;
    case 'M': m_buffer.DeleteLines(p(0)); break;
    case 'P': m_buffer.DeleteChars(p(0)); break;
    case 'S': m_buffer.ScrollUp(p(0)); break;
    case 'T': m_buffer.ScrollDown(p(0)); break;
    case 'X': m_buffer.EraseChars(p(0)); break;
    case 'Z': // CBT - Cursor Backward Tab
        m_buffer.TabBackward(p(0));
        break;
    case '@': m_buffer.InsertChars(p(0)); break;
    case 'b': // REP - Repeat previous character
        m_buffer.RepeatLastChar(p(0));
        break;
    case 'd': m_buffer.SetCursorRow(p(0) - 1); break;
    case 'g': // TBC - Tab Clear
        m_buffer.ClearTabStop(p(0, 0));
        break;
    case 'h': // SM - Set Mode (non-private)
        // Accept silently - most non-private modes are not relevant
        break;
    case 'l': // RM - Reset Mode (non-private)
        break;
    case 'm': HandleSgr(); break;
    case 'r': { // DECSTBM
        int top = p(0) - 1;
        int bot = (m_params.size() > 1 && m_params[1] > 0)
                  ? m_params[1] - 1 : m_buffer.GetRows() - 1;
        m_buffer.SetScrollRegion(top, bot);
        break;
    }
    case 's': m_buffer.SaveCursor(); break;
    case 'u': m_buffer.RestoreCursor(); break;
    case 'n': // DSR - Device Status Report
        if (p(0, 0) == 6) {
            char resp[32];
            int len = snprintf(resp, sizeof(resp), "\x1b[%d;%dR",
                               m_buffer.GetCursorRow() + 1,
                               m_buffer.GetCursorCol() + 1);
            WriteBack(resp, len);
        } else if (p(0, 0) == 5) {
            WriteBack("\x1b[0n", 4); // terminal OK
        }
        break;
    case 'c': // DA - Device Attributes (Primary)
        if (!m_privateMarker) {
            WriteBack("\x1b[?62;22c", 9);
        }
        break;
    case 't': // Window manipulation
        break; // Ignore
    }
}

void VtParser::HandleSgr() {
    if (m_params.empty()) {
        m_buffer.ResetAttributes();
        return;
    }

    for (size_t i = 0; i < m_params.size(); i++) {
        int p = m_params[i];

        // Check for colon sub-parameters (e.g., 4:2 for double underline, 38:2:r:g:b)
        const auto& subs = (i < m_subParams.size()) ? m_subParams[i] : std::vector<int>{};

        switch (p) {
        case 0: m_buffer.ResetAttributes(); break;
        case 1: m_buffer.SetBold(true); break;
        case 2: m_buffer.SetDim(true); break;
        case 3: m_buffer.SetItalic(true); break;
        case 4: // Underline (with optional sub-parameter for style)
            if (!subs.empty()) {
                int style = subs[0];
                if (style == 0) {
                    m_buffer.SetUnderline(false);
                } else {
                    m_buffer.SetUnderlineStyle(static_cast<uint8_t>(
                        style <= 5 ? style - 1 : 0));
                }
            } else {
                m_buffer.SetUnderline(true);
            }
            break;
        case 5: m_buffer.SetBlink(true); break;
        case 6: m_buffer.SetBlink(true); break; // Rapid blink = same as blink
        case 7: m_buffer.SetInverse(true); break;
        case 8: m_buffer.SetConceal(true); break;
        case 9: m_buffer.SetStrikethrough(true); break;
        case 21: m_buffer.SetUnderlineStyle(UL_DOUBLE); break;
        case 22: m_buffer.SetBold(false); m_buffer.SetDim(false); break;
        case 23: m_buffer.SetItalic(false); break;
        case 24: m_buffer.SetUnderline(false); break;
        case 25: m_buffer.SetBlink(false); break;
        case 27: m_buffer.SetInverse(false); break;
        case 28: m_buffer.SetConceal(false); break;
        case 29: m_buffer.SetStrikethrough(false); break;

        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            m_buffer.SetFg(static_cast<uint8_t>(p - 30));
            break;

        case 38: // Extended foreground
            if (!subs.empty()) {
                // Colon-separated: 38:5:idx or 38:2:r:g:b or 38:2:cs:r:g:b
                if (subs[0] == 5 && subs.size() >= 2) {
                    m_buffer.SetFg(static_cast<uint8_t>(subs[1]));
                } else if (subs[0] == 2) {
                    if (subs.size() >= 4) {
                        // 38:2:r:g:b or 38:2:cs:r:g:b
                        size_t off = (subs.size() >= 5) ? 2 : 1;
                        m_buffer.SetFgRgb(
                            static_cast<uint8_t>(subs[off]),
                            static_cast<uint8_t>(subs[off + 1]),
                            static_cast<uint8_t>(subs[off + 2]));
                    }
                }
            } else if (i + 1 < m_params.size()) {
                // Semicolon-separated: 38;5;idx or 38;2;r;g;b
                if (m_params[i + 1] == 5 && i + 2 < m_params.size()) {
                    m_buffer.SetFg(static_cast<uint8_t>(m_params[i + 2]));
                    i += 2;
                } else if (m_params[i + 1] == 2 && i + 4 < m_params.size()) {
                    m_buffer.SetFgRgb(
                        static_cast<uint8_t>(m_params[i + 2]),
                        static_cast<uint8_t>(m_params[i + 3]),
                        static_cast<uint8_t>(m_params[i + 4]));
                    i += 4;
                }
            }
            break;

        case 39: m_buffer.SetDefaultFg(); break;

        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            m_buffer.SetBg(static_cast<uint8_t>(p - 40));
            break;

        case 48: // Extended background
            if (!subs.empty()) {
                if (subs[0] == 5 && subs.size() >= 2) {
                    m_buffer.SetBg(static_cast<uint8_t>(subs[1]));
                } else if (subs[0] == 2) {
                    if (subs.size() >= 4) {
                        size_t off = (subs.size() >= 5) ? 2 : 1;
                        m_buffer.SetBgRgb(
                            static_cast<uint8_t>(subs[off]),
                            static_cast<uint8_t>(subs[off + 1]),
                            static_cast<uint8_t>(subs[off + 2]));
                    }
                }
            } else if (i + 1 < m_params.size()) {
                if (m_params[i + 1] == 5 && i + 2 < m_params.size()) {
                    m_buffer.SetBg(static_cast<uint8_t>(m_params[i + 2]));
                    i += 2;
                } else if (m_params[i + 1] == 2 && i + 4 < m_params.size()) {
                    m_buffer.SetBgRgb(
                        static_cast<uint8_t>(m_params[i + 2]),
                        static_cast<uint8_t>(m_params[i + 3]),
                        static_cast<uint8_t>(m_params[i + 4]));
                    i += 4;
                }
            }
            break;

        case 49: m_buffer.SetDefaultBg(); break;

        case 53: m_buffer.SetOverline(true); break;
        case 55: m_buffer.SetOverline(false); break;

        case 58: // Underline color - accept silently (no storage yet)
            if (!subs.empty()) {
                // 58:2:r:g:b or 58:5:idx - skip sub-params
            } else if (i + 1 < m_params.size()) {
                if (m_params[i + 1] == 5 && i + 2 < m_params.size()) i += 2;
                else if (m_params[i + 1] == 2 && i + 4 < m_params.size()) i += 4;
            }
            break;
        case 59: // Default underline color - accept silently
            break;

        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            m_buffer.SetFg(static_cast<uint8_t>(p - 90 + 8));
            break;

        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            m_buffer.SetBg(static_cast<uint8_t>(p - 100 + 8));
            break;
        }
    }
}

void VtParser::HandleDecMode(uint8_t action) {
    bool enable = (action == 'h');
    for (int p : m_params) {
        m_buffer.SetMode(p, enable);
    }
}

void VtParser::EmitCodepoint(uint32_t cp) {
    if (cp <= 0xFFFF) {
        m_buffer.PutChar(static_cast<wchar_t>(cp));
    } else if (cp <= 0x10FFFF) {
        uint32_t adj = cp - 0x10000;
        wchar_t hi = static_cast<wchar_t>(0xD800 + (adj >> 10));
        wchar_t lo = static_cast<wchar_t>(0xDC00 + (adj & 0x3FF));
        m_buffer.PutCharPair(hi, lo);
    }
}

void VtParser::WriteBack(const char* data, size_t len) {
    if (m_writeCb)
        m_writeCb(data, len, m_writeCtx);
}
