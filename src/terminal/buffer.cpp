#include "terminal/buffer.h"

void TerminalBuffer::Init(int cols, int rows) {
    m_cols = cols;
    m_rows = rows;
    m_cells.assign(static_cast<size_t>(cols) * rows, Cell{});
    m_cursorRow = 0;
    m_cursorCol = 0;
    m_scrollTop = 0;
    m_scrollBottom = rows - 1;
    m_wrapPending = false;
    m_attr = Cell{};
    m_lastChar = 0;
    m_autoWrap = true;
    m_originMode = false;
    InitTabStops();
}

void TerminalBuffer::InitTabStops() {
    m_tabStops.reset();
    for (int c = 0; c < 512; c += 8)
        m_tabStops.set(c);
}

TerminalBufferSnapshot TerminalBuffer::CreateSnapshot() const {
    TerminalBufferSnapshot snapshot;
    snapshot.cells = m_cells;
    snapshot.cols = m_cols;
    snapshot.rows = m_rows;
    snapshot.cursorRow = m_cursorRow;
    snapshot.cursorCol = m_cursorCol;
    snapshot.wrapPending = m_wrapPending;
    snapshot.attr = m_attr;
    snapshot.scrollTop = m_scrollTop;
    snapshot.scrollBottom = m_scrollBottom;
    snapshot.cursorVisible = m_cursorVisible;
    snapshot.appCursorKeys = m_appCursorKeys;
    snapshot.bracketedPaste = m_bracketedPaste;
    snapshot.title = m_title;
    snapshot.savedCursorRow = m_savedCursorRow;
    snapshot.savedCursorCol = m_savedCursorCol;
    snapshot.savedAttr = m_savedAttr;
    snapshot.scrollback = m_scrollback;
    snapshot.scrollOffset = m_scrollOffset;
    snapshot.maxScrollback = m_maxScrollback;
    snapshot.altScreenActive = m_altScreenActive;
    snapshot.savedMainBuffer = m_savedMainBuffer;
    snapshot.savedMainCursorRow = m_savedMainCursorRow;
    snapshot.savedMainCursorCol = m_savedMainCursorCol;
    snapshot.savedMainAttr = m_savedMainAttr;
    return snapshot;
}

void TerminalBuffer::LoadSnapshot(const TerminalBufferSnapshot& snapshot) {
    int savedScrollOffset = m_scrollOffset;
    m_cells = snapshot.cells;
    m_cols = snapshot.cols;
    m_rows = snapshot.rows;
    m_cursorRow = snapshot.cursorRow;
    m_cursorCol = snapshot.cursorCol;
    m_wrapPending = snapshot.wrapPending;
    m_attr = snapshot.attr;
    m_scrollTop = snapshot.scrollTop;
    m_scrollBottom = snapshot.scrollBottom;
    m_cursorVisible = snapshot.cursorVisible;
    m_appCursorKeys = snapshot.appCursorKeys;
    m_bracketedPaste = snapshot.bracketedPaste;
    m_title = snapshot.title;
    m_savedCursorRow = snapshot.savedCursorRow;
    m_savedCursorCol = snapshot.savedCursorCol;
    m_savedAttr = snapshot.savedAttr;
    m_scrollback = snapshot.scrollback;
    if (savedScrollOffset > 0) {
        m_scrollOffset = (std::min)(savedScrollOffset, static_cast<int>(m_scrollback.size()));
    } else {
        m_scrollOffset = snapshot.scrollOffset;
    }
    m_maxScrollback = snapshot.maxScrollback;
    m_altScreenActive = snapshot.altScreenActive;
    m_savedMainBuffer = snapshot.savedMainBuffer;
    m_savedMainCursorRow = snapshot.savedMainCursorRow;
    m_savedMainCursorCol = snapshot.savedMainCursorCol;
    m_savedMainAttr = snapshot.savedMainAttr;
}

void TerminalBuffer::Resize(int newCols, int newRows) {
    if (newCols == m_cols && newRows == m_rows)
        return;

    if (newRows < m_rows && !m_altScreenActive) {
        int lastContentRow = m_rows - 1;
        while (lastContentRow > m_cursorRow) {
            bool empty = true;
            for (int c = 0; c < m_cols; c++) {
                const Cell& cell = At(lastContentRow, c);
                if (cell.ch != L' ' && cell.ch != 0 && cell.width > 0) {
                    empty = false;
                    break;
                }
            }
            if (!empty) break;
            lastContentRow--;
        }

        int contentRows = lastContentRow + 1;
        int excessRows = contentRows - newRows;
        if (excessRows > 0) {
            for (int r = 0; r < excessRows; r++) {
                std::vector<Cell> line(m_cols);
                for (int c = 0; c < m_cols; c++)
                    line[c] = At(r, c);
                m_scrollback.push_back(std::move(line));
                if (static_cast<int>(m_scrollback.size()) > m_maxScrollback)
                    m_scrollback.pop_front();
            }

            for (int r = 0; r < m_rows - excessRows; r++) {
                for (int c = 0; c < m_cols; c++)
                    At(r, c) = At(r + excessRows, c);
            }

            m_cursorRow -= excessRows;
            if (m_cursorRow < 0) m_cursorRow = 0;
            m_savedCursorRow -= excessRows;
            if (m_savedCursorRow < 0) m_savedCursorRow = 0;
        }
    }

    std::vector<Cell> newCells(static_cast<size_t>(newCols) * newRows, Cell{});
    int copyRows = (std::min)(m_rows, newRows);
    int copyCols = (std::min)(m_cols, newCols);

    for (int r = 0; r < copyRows; r++) {
        for (int c = 0; c < copyCols; c++) {
            newCells[r * newCols + c] = m_cells[r * m_cols + c];
        }
    }

    m_cells = std::move(newCells);
    m_cols = newCols;
    m_rows = newRows;
    m_cursorRow = (std::min)(m_cursorRow, m_rows - 1);
    m_cursorCol = (std::min)(m_cursorCol, m_cols - 1);
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_wrapPending = false;

    if (m_altScreenActive) {
        m_savedMainBuffer.resize(static_cast<size_t>(newCols) * newRows, Cell{});
    }
}

int TerminalBuffer::CharWidth(wchar_t ch) {
    if (ch < 0x1100) return 1;
    // Hangul Jamo
    if (ch <= 0x115F) return 2;
    // CJK Radicals, Kangxi, CJK Symbols
    if (ch >= 0x2E80 && ch <= 0x303E) return 2;
    // Hiragana, Katakana, Bopomofo, Hangul Compat Jamo, CJK misc
    if (ch >= 0x3041 && ch <= 0x33BF) return 2;
    // CJK Unified Ideographs Extension A
    if (ch >= 0x3400 && ch <= 0x4DBF) return 2;
    // CJK Unified Ideographs, Yi
    if (ch >= 0x4E00 && ch <= 0xA4CF) return 2;
    // Hangul Jamo Extended-A
    if (ch >= 0xA960 && ch <= 0xA97C) return 2;
    // Hangul Syllables
    if (ch >= 0xAC00 && ch <= 0xD7A3) return 2;
    // CJK Compatibility Ideographs
    if (ch >= 0xF900 && ch <= 0xFAFF) return 2;
    // Vertical Forms, CJK Compatibility Forms
    if (ch >= 0xFE10 && ch <= 0xFE6F) return 2;
    // Fullwidth Forms
    if (ch >= 0xFF01 && ch <= 0xFF60) return 2;
    if (ch >= 0xFFE0 && ch <= 0xFFE6) return 2;
    return 1;
}

void TerminalBuffer::PutChar(wchar_t ch) {
    int w = CharWidth(ch);

    if (m_wrapPending) {
        if (m_autoWrap) {
            m_cursorCol = 0;
            LineFeed();
        }
        m_wrapPending = false;
    }

    // Wide char doesn't fit at last column: pad with space and wrap
    if (w == 2 && m_cursorCol >= m_cols - 1) {
        ClearCell(At(m_cursorRow, m_cursorCol));
        if (m_autoWrap) {
            m_cursorCol = 0;
            LineFeed();
        }
    }

    // Overwriting the trailing half of an existing wide char: clear lead
    if (m_cursorCol > 0 && At(m_cursorRow, m_cursorCol).width == 0) {
        ClearCell(At(m_cursorRow, m_cursorCol - 1));
    }

    // Overwriting the leading half of an existing wide char: clear trail
    if (At(m_cursorRow, m_cursorCol).width == 2 && m_cursorCol + 1 < m_cols) {
        ClearCell(At(m_cursorRow, m_cursorCol + 1));
    }

    // Wide char overwrites a wide lead at cursorCol+1
    if (w == 2 && m_cursorCol + 1 < m_cols) {
        Cell& next = At(m_cursorRow, m_cursorCol + 1);
        if (next.width == 2 && m_cursorCol + 2 < m_cols) {
            ClearCell(At(m_cursorRow, m_cursorCol + 2));
        }
    }

    // Place the character
    Cell& cell = At(m_cursorRow, m_cursorCol);
    cell.ch = ch;
    cell.ch2 = 0;
    cell.fg = m_attr.fg;
    cell.bg = m_attr.bg;
    cell.flags = m_attr.flags;
    cell.flags2 = m_attr.flags2;
    cell.fgRgb = m_attr.fgRgb;
    cell.bgRgb = m_attr.bgRgb;
    cell.width = static_cast<uint8_t>(w);

    if (w == 2 && m_cursorCol + 1 < m_cols) {
        Cell& trail = At(m_cursorRow, m_cursorCol + 1);
        trail.ch = L' ';
        trail.ch2 = 0;
        trail.fg = m_attr.fg;
        trail.bg = m_attr.bg;
        trail.flags = m_attr.flags;
        trail.flags2 = m_attr.flags2;
        trail.fgRgb = m_attr.fgRgb;
        trail.bgRgb = m_attr.bgRgb;
        trail.width = 0;
    }

    m_lastChar = ch;

    // Advance cursor
    if (m_cursorCol + w < m_cols) {
        m_cursorCol += w;
    } else {
        m_wrapPending = true;
    }
}

void TerminalBuffer::PutCharPair(wchar_t hi, wchar_t lo) {
    // Supplementary Unicode character (surrogate pair) - always width 2
    if (m_wrapPending) {
        if (m_autoWrap) {
            m_cursorCol = 0;
            LineFeed();
        }
        m_wrapPending = false;
    }

    if (m_cursorCol >= m_cols - 1) {
        ClearCell(At(m_cursorRow, m_cursorCol));
        if (m_autoWrap) {
            m_cursorCol = 0;
            LineFeed();
        }
    }

    if (m_cursorCol > 0 && At(m_cursorRow, m_cursorCol).width == 0)
        ClearCell(At(m_cursorRow, m_cursorCol - 1));
    if (At(m_cursorRow, m_cursorCol).width == 2 && m_cursorCol + 1 < m_cols)
        ClearCell(At(m_cursorRow, m_cursorCol + 1));
    if (m_cursorCol + 1 < m_cols) {
        Cell& next = At(m_cursorRow, m_cursorCol + 1);
        if (next.width == 2 && m_cursorCol + 2 < m_cols)
            ClearCell(At(m_cursorRow, m_cursorCol + 2));
    }

    Cell& cell = At(m_cursorRow, m_cursorCol);
    cell.ch = hi;
    cell.ch2 = lo;
    cell.fg = m_attr.fg;
    cell.bg = m_attr.bg;
    cell.flags = m_attr.flags;
    cell.flags2 = m_attr.flags2;
    cell.fgRgb = m_attr.fgRgb;
    cell.bgRgb = m_attr.bgRgb;
    cell.width = 2;

    if (m_cursorCol + 1 < m_cols) {
        Cell& trail = At(m_cursorRow, m_cursorCol + 1);
        trail.ch = L' ';
        trail.ch2 = 0;
        trail.fg = m_attr.fg;
        trail.bg = m_attr.bg;
        trail.flags = m_attr.flags;
        trail.flags2 = m_attr.flags2;
        trail.fgRgb = m_attr.fgRgb;
        trail.bgRgb = m_attr.bgRgb;
        trail.width = 0;
    }

    m_lastChar = 0;

    if (m_cursorCol + 2 < m_cols)
        m_cursorCol += 2;
    else
        m_wrapPending = true;
}

void TerminalBuffer::RepeatLastChar(int n) {
    if (m_lastChar == 0) return;
    for (int i = 0; i < n; i++)
        PutChar(m_lastChar);
}

void TerminalBuffer::LineFeed() {
    m_wrapPending = false;
    if (m_cursorRow == m_scrollBottom) {
        ScrollUp(1);
    } else if (m_cursorRow < m_rows - 1) {
        m_cursorRow++;
    }
}

void TerminalBuffer::CarriageReturn() {
    m_cursorCol = 0;
    m_wrapPending = false;
}

void TerminalBuffer::Backspace() {
    if (m_cursorCol > 0)
        m_cursorCol--;
    m_wrapPending = false;
}

void TerminalBuffer::Tab() {
    for (int c = m_cursorCol + 1; c < m_cols; c++) {
        if (m_tabStops.test(c)) {
            m_cursorCol = c;
            m_wrapPending = false;
            return;
        }
    }
    m_cursorCol = m_cols - 1;
    m_wrapPending = false;
}

void TerminalBuffer::TabForward(int n) {
    for (int i = 0; i < n; i++)
        Tab();
}

void TerminalBuffer::TabBackward(int n) {
    for (int i = 0; i < n; i++) {
        for (int c = m_cursorCol - 1; c >= 0; c--) {
            if (m_tabStops.test(c)) {
                m_cursorCol = c;
                break;
            }
            if (c == 0) {
                m_cursorCol = 0;
            }
        }
    }
    m_wrapPending = false;
}

void TerminalBuffer::SetTabStop() {
    if (m_cursorCol < 512)
        m_tabStops.set(m_cursorCol);
}

void TerminalBuffer::ClearTabStop(int mode) {
    if (mode == 0) {
        if (m_cursorCol < 512)
            m_tabStops.reset(m_cursorCol);
    } else if (mode == 3) {
        m_tabStops.reset();
    }
}

void TerminalBuffer::ReverseIndex() {
    if (m_cursorRow == m_scrollTop) {
        ScrollDown(1);
    } else if (m_cursorRow > 0) {
        m_cursorRow--;
    }
}

void TerminalBuffer::Index() {
    LineFeed();
}

void TerminalBuffer::NextLine() {
    CarriageReturn();
    LineFeed();
}

void TerminalBuffer::SetCursorPos(int row, int col) {
    if (m_originMode) {
        row += m_scrollTop;
        row = (std::max)(m_scrollTop, (std::min)(row, m_scrollBottom));
    } else {
        row = (std::max)(0, (std::min)(row, m_rows - 1));
    }
    m_cursorRow = row;
    m_cursorCol = (std::max)(0, (std::min)(col, m_cols - 1));
    m_wrapPending = false;
}

void TerminalBuffer::MoveCursorUp(int n) {
    m_cursorRow = (std::max)(m_scrollTop, m_cursorRow - n);
    m_wrapPending = false;
}

void TerminalBuffer::MoveCursorDown(int n) {
    m_cursorRow = (std::min)(m_scrollBottom, m_cursorRow + n);
    m_wrapPending = false;
}

void TerminalBuffer::MoveCursorForward(int n) {
    m_cursorCol = (std::min)(m_cols - 1, m_cursorCol + n);
    m_wrapPending = false;
}

void TerminalBuffer::MoveCursorBack(int n) {
    m_cursorCol = (std::max)(0, m_cursorCol - n);
    m_wrapPending = false;
}

void TerminalBuffer::SetCursorCol(int col) {
    m_cursorCol = (std::max)(0, (std::min)(col, m_cols - 1));
    m_wrapPending = false;
}

void TerminalBuffer::SetCursorRow(int row) {
    if (m_originMode) {
        row += m_scrollTop;
        m_cursorRow = (std::max)(m_scrollTop, (std::min)(row, m_scrollBottom));
    } else {
        m_cursorRow = (std::max)(0, (std::min)(row, m_rows - 1));
    }
    m_wrapPending = false;
}

void TerminalBuffer::SaveCursor() {
    m_savedCursorRow = m_cursorRow;
    m_savedCursorCol = m_cursorCol;
    m_savedAttr = m_attr;
}

void TerminalBuffer::RestoreCursor() {
    m_cursorRow = m_savedCursorRow;
    m_cursorCol = m_savedCursorCol;
    m_attr = m_savedAttr;
    ClampCursor();
}

void TerminalBuffer::EraseDisplay(int mode) {
    switch (mode) {
    case 0: // Erase below
        for (int c = m_cursorCol; c < m_cols; c++)
            ClearCell(At(m_cursorRow, c));
        for (int r = m_cursorRow + 1; r < m_rows; r++)
            ClearLine(r);
        break;
    case 1: // Erase above
        for (int r = 0; r < m_cursorRow; r++)
            ClearLine(r);
        for (int c = 0; c <= m_cursorCol; c++)
            ClearCell(At(m_cursorRow, c));
        break;
    case 2: // Erase all
    case 3: // Erase all + scrollback
        for (int r = 0; r < m_rows; r++)
            ClearLine(r);
        if (mode == 3) {
            m_scrollback.clear();
            m_scrollOffset = 0;
        }
        break;
    }
}

void TerminalBuffer::EraseLine(int mode) {
    switch (mode) {
    case 0: // Erase to right
        for (int c = m_cursorCol; c < m_cols; c++)
            ClearCell(At(m_cursorRow, c));
        break;
    case 1: // Erase to left
        for (int c = 0; c <= m_cursorCol; c++)
            ClearCell(At(m_cursorRow, c));
        break;
    case 2: // Erase entire line
        ClearLine(m_cursorRow);
        break;
    }
}

void TerminalBuffer::EraseChars(int n) {
    for (int i = 0; i < n && m_cursorCol + i < m_cols; i++)
        ClearCell(At(m_cursorRow, m_cursorCol + i));
}

void TerminalBuffer::InsertLines(int n) {
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom)
        return;
    for (int i = 0; i < n; i++) {
        for (int r = m_scrollBottom; r > m_cursorRow; r--) {
            for (int c = 0; c < m_cols; c++)
                At(r, c) = At(r - 1, c);
        }
        ClearLine(m_cursorRow);
    }
}

void TerminalBuffer::DeleteLines(int n) {
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom)
        return;
    for (int i = 0; i < n; i++) {
        for (int r = m_cursorRow; r < m_scrollBottom; r++) {
            for (int c = 0; c < m_cols; c++)
                At(r, c) = At(r + 1, c);
        }
        ClearLine(m_scrollBottom);
    }
}

void TerminalBuffer::DeleteChars(int n) {
    for (int i = 0; i < n; i++) {
        for (int c = m_cursorCol; c < m_cols - 1; c++)
            At(m_cursorRow, c) = At(m_cursorRow, c + 1);
        ClearCell(At(m_cursorRow, m_cols - 1));
    }
}

void TerminalBuffer::InsertChars(int n) {
    for (int i = 0; i < n; i++) {
        for (int c = m_cols - 1; c > m_cursorCol; c--)
            At(m_cursorRow, c) = At(m_cursorRow, c - 1);
        ClearCell(At(m_cursorRow, m_cursorCol));
    }
}

void TerminalBuffer::ScrollUp(int n) {
    for (int i = 0; i < n; i++) {
        // Save top line to scrollback (only for full-screen scroll, not alt screen)
        if (m_scrollTop == 0 && !m_altScreenActive) {
            std::vector<Cell> line(m_cols);
            for (int c = 0; c < m_cols; c++)
                line[c] = At(0, c);
            m_scrollback.push_back(std::move(line));
            if (m_scrollOffset > 0)
                m_scrollOffset++;
            if (static_cast<int>(m_scrollback.size()) > m_maxScrollback) {
                m_scrollback.pop_front();
                if (m_scrollOffset > 0)
                    m_scrollOffset--;
            }
        }

        for (int r = m_scrollTop; r < m_scrollBottom; r++) {
            for (int c = 0; c < m_cols; c++)
                At(r, c) = At(r + 1, c);
        }
        ClearLine(m_scrollBottom);
    }
}

void TerminalBuffer::ScrollDown(int n) {
    for (int i = 0; i < n; i++) {
        for (int r = m_scrollBottom; r > m_scrollTop; r--) {
            for (int c = 0; c < m_cols; c++)
                At(r, c) = At(r - 1, c);
        }
        ClearLine(m_scrollTop);
    }
}

void TerminalBuffer::SetScrollRegion(int top, int bottom) {
    m_scrollTop = (std::max)(0, (std::min)(top, m_rows - 1));
    m_scrollBottom = (std::max)(m_scrollTop, (std::min)(bottom, m_rows - 1));
    SetCursorPos(0, 0);
}

void TerminalBuffer::ResetAttributes() {
    m_attr = Cell{};
}

void TerminalBuffer::SetBold(bool v) {
    if (v) m_attr.flags |= CELL_BOLD;
    else   m_attr.flags &= ~CELL_BOLD;
}

void TerminalBuffer::SetItalic(bool v) {
    if (v) m_attr.flags |= CELL_ITALIC;
    else   m_attr.flags &= ~CELL_ITALIC;
}

void TerminalBuffer::SetUnderline(bool v) {
    if (v) m_attr.flags |= CELL_UNDERLINE;
    else {
        m_attr.flags &= ~CELL_UNDERLINE;
        m_attr.flags2 &= ~CELL_UL_STYLE_MASK;
    }
}

void TerminalBuffer::SetInverse(bool v) {
    if (v) m_attr.flags |= CELL_INVERSE;
    else   m_attr.flags &= ~CELL_INVERSE;
}

void TerminalBuffer::SetDim(bool v) {
    if (v) m_attr.flags2 |= CELL_DIM;
    else   m_attr.flags2 &= ~CELL_DIM;
}

void TerminalBuffer::SetStrikethrough(bool v) {
    if (v) m_attr.flags2 |= CELL_STRIKETHROUGH;
    else   m_attr.flags2 &= ~CELL_STRIKETHROUGH;
}

void TerminalBuffer::SetConceal(bool v) {
    if (v) m_attr.flags2 |= CELL_CONCEAL;
    else   m_attr.flags2 &= ~CELL_CONCEAL;
}

void TerminalBuffer::SetOverline(bool v) {
    if (v) m_attr.flags2 |= CELL_OVERLINE;
    else   m_attr.flags2 &= ~CELL_OVERLINE;
}

void TerminalBuffer::SetBlink(bool v) {
    if (v) m_attr.flags2 |= CELL_BLINK;
    else   m_attr.flags2 &= ~CELL_BLINK;
}

void TerminalBuffer::SetUnderlineStyle(uint8_t style) {
    m_attr.flags |= CELL_UNDERLINE;
    m_attr.flags2 = (m_attr.flags2 & ~CELL_UL_STYLE_MASK) |
                    ((style & 0x07) << 5);
}

void TerminalBuffer::SetFg(uint8_t idx) {
    m_attr.fg = idx;
    m_attr.flags &= ~(CELL_FG_RGB | CELL_FG_DEFAULT);
}

void TerminalBuffer::SetBg(uint8_t idx) {
    m_attr.bg = idx;
    m_attr.flags &= ~(CELL_BG_RGB | CELL_BG_DEFAULT);
}

void TerminalBuffer::SetFgRgb(uint8_t r, uint8_t g, uint8_t b) {
    m_attr.fgRgb = (static_cast<uint32_t>(r) << 16) |
                   (static_cast<uint32_t>(g) << 8) | b;
    m_attr.flags |= CELL_FG_RGB;
    m_attr.flags &= ~CELL_FG_DEFAULT;
}

void TerminalBuffer::SetBgRgb(uint8_t r, uint8_t g, uint8_t b) {
    m_attr.bgRgb = (static_cast<uint32_t>(r) << 16) |
                   (static_cast<uint32_t>(g) << 8) | b;
    m_attr.flags |= CELL_BG_RGB;
    m_attr.flags &= ~CELL_BG_DEFAULT;
}

void TerminalBuffer::SetDefaultFg() {
    m_attr.flags |= CELL_FG_DEFAULT;
    m_attr.flags &= ~CELL_FG_RGB;
}

void TerminalBuffer::SetDefaultBg() {
    m_attr.flags |= CELL_BG_DEFAULT;
    m_attr.flags &= ~CELL_BG_RGB;
}

void TerminalBuffer::SetMode(int mode, bool enabled) {
    switch (mode) {
    case 1:
        m_appCursorKeys = enabled;
        break;
    case 6:
        m_originMode = enabled;
        if (enabled)
            SetCursorPos(0, 0);
        break;
    case 7:
        m_autoWrap = enabled;
        break;
    case 25:
        m_cursorVisible = enabled;
        break;
    case 1047:
        if (enabled && !m_altScreenActive) {
            m_savedMainBuffer = m_cells;
            m_cells.assign(static_cast<size_t>(m_cols) * m_rows, Cell{});
            m_altScreenActive = true;
        } else if (!enabled && m_altScreenActive) {
            m_cells = m_savedMainBuffer;
            m_altScreenActive = false;
            ClampCursor();
        }
        break;
    case 1048:
        if (enabled) SaveCursor();
        else RestoreCursor();
        break;
    case 1049:
        if (enabled && !m_altScreenActive) {
            m_savedMainBuffer = m_cells;
            m_savedMainCursorRow = m_cursorRow;
            m_savedMainCursorCol = m_cursorCol;
            m_savedMainAttr = m_attr;
            m_cells.assign(static_cast<size_t>(m_cols) * m_rows, Cell{});
            m_cursorRow = 0;
            m_cursorCol = 0;
            m_altScreenActive = true;
        } else if (!enabled && m_altScreenActive) {
            m_cells = m_savedMainBuffer;
            m_cursorRow = m_savedMainCursorRow;
            m_cursorCol = m_savedMainCursorCol;
            m_attr = m_savedMainAttr;
            m_altScreenActive = false;
            ClampCursor();
        }
        break;
    case 2004:
        m_bracketedPaste = enabled;
        break;
    case 2026:
        // Synchronized output: accepted silently
        break;
    }
}

void TerminalBuffer::FullReset() {
    m_cursorRow = 0;
    m_cursorCol = 0;
    m_wrapPending = false;
    m_attr = Cell{};
    m_lastChar = 0;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_cursorVisible = true;
    m_appCursorKeys = false;
    m_bracketedPaste = false;
    m_autoWrap = true;
    m_originMode = false;
    m_savedCursorRow = 0;
    m_savedCursorCol = 0;
    m_savedAttr = Cell{};
    m_altScreenActive = false;
    m_savedMainBuffer.clear();
    m_scrollback.clear();
    m_scrollOffset = 0;
    InitTabStops();
    for (int r = 0; r < m_rows; r++)
        ClearLine(r);
}

void TerminalBuffer::SoftReset() {
    m_cursorVisible = true;
    m_originMode = false;
    m_autoWrap = true;
    m_appCursorKeys = false;
    m_bracketedPaste = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_attr = Cell{};
    m_savedCursorRow = 0;
    m_savedCursorCol = 0;
    m_savedAttr = Cell{};
    InitTabStops();
}

void TerminalBuffer::ClearLine(int row) {
    for (int c = 0; c < m_cols; c++)
        ClearCell(At(row, c));
}

void TerminalBuffer::ClearCell(Cell& cell) {
    cell = Cell{};
}

void TerminalBuffer::ClampCursor() {
    m_cursorRow = (std::max)(0, (std::min)(m_cursorRow, m_rows - 1));
    m_cursorCol = (std::max)(0, (std::min)(m_cursorCol, m_cols - 1));
}

const Cell& TerminalBuffer::ViewAt(int viewRow, int col) const {
    int documentRow = static_cast<int>(m_scrollback.size()) - m_scrollOffset + viewRow;
    return CellAtDocumentRow(documentRow, col);
}

const Cell& TerminalBuffer::CellAtDocumentRow(int documentRow, int col) const {
    static const Cell empty{};
    if (documentRow < 0 || col < 0 || col >= m_cols)
        return empty;

    int sbSize = static_cast<int>(m_scrollback.size());
    if (documentRow < sbSize) {
        auto& line = m_scrollback[documentRow];
        if (col < static_cast<int>(line.size()))
            return line[col];
        return empty;
    }

    int bufRow = documentRow - sbSize;
    if (bufRow >= 0 && bufRow < m_rows)
        return At(bufRow, col);

    return empty;
}

int TerminalBuffer::ViewRowToDocumentRow(int viewRow) const {
    if (m_rows <= 0)
        return 0;

    int clampedViewRow = (std::max)(0, (std::min)(viewRow, m_rows - 1));
    int firstVisibleRow = static_cast<int>(m_scrollback.size()) - m_scrollOffset;
    int documentRow = firstVisibleRow + clampedViewRow;
    int maxDocumentRow = GetDocumentRowCount() - 1;
    return (std::max)(0, (std::min)(documentRow, maxDocumentRow));
}

int TerminalBuffer::DocumentRowToViewRow(int documentRow) const {
    int firstVisibleRow = static_cast<int>(m_scrollback.size()) - m_scrollOffset;
    return documentRow - firstVisibleRow;
}

void TerminalBuffer::ScrollBack(int lines) {
    int maxOffset = static_cast<int>(m_scrollback.size());
    m_scrollOffset = (std::min)(m_scrollOffset + lines, maxOffset);
}

void TerminalBuffer::ScrollForward(int lines) {
    m_scrollOffset = (std::max)(0, m_scrollOffset - lines);
}

void TerminalBuffer::ScrollToBottom() {
    m_scrollOffset = 0;
}
