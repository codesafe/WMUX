#pragma once
#include <vector>
#include <deque>
#include <string>
#include <cstdint>
#include <algorithm>

enum CellFlags : uint8_t {
    CELL_BOLD       = 1 << 0,
    CELL_ITALIC     = 1 << 1,
    CELL_UNDERLINE  = 1 << 2,
    CELL_INVERSE    = 1 << 3,
    CELL_FG_RGB     = 1 << 4,
    CELL_BG_RGB     = 1 << 5,
    CELL_FG_DEFAULT = 1 << 6,
    CELL_BG_DEFAULT = 1 << 7,
};

struct Cell {
    wchar_t ch = L' ';
    wchar_t ch2 = 0;    // surrogate pair low half (0 if unused)
    uint8_t fg = 7;
    uint8_t bg = 0;
    uint8_t flags = CELL_FG_DEFAULT | CELL_BG_DEFAULT;
    uint32_t fgRgb = 0;
    uint32_t bgRgb = 0;
    uint8_t width = 1;  // 1=normal, 2=wide char lead, 0=wide char trail
};

class TerminalBuffer {
public:
    TerminalBuffer() = default;
    void Init(int cols, int rows);
    void Resize(int cols, int rows);

    int GetCols() const { return m_cols; }
    int GetRows() const { return m_rows; }
    int GetCursorRow() const { return m_cursorRow; }
    int GetCursorCol() const { return m_cursorCol; }
    bool IsCursorVisible() const { return m_cursorVisible; }
    bool IsAppCursorKeys() const { return m_appCursorKeys; }
    bool IsBracketedPaste() const { return m_bracketedPaste; }

    void SetTitle(const std::string& title) { m_title = title; }
    const std::string& GetTitle() const { return m_title; }

    const Cell& At(int row, int col) const { return m_cells[row * m_cols + col]; }
    Cell& At(int row, int col) { return m_cells[row * m_cols + col]; }

    // Character width
    static int CharWidth(wchar_t ch);

    // Character output
    void PutChar(wchar_t ch);
    void PutCharPair(wchar_t hi, wchar_t lo);
    void LineFeed();
    void CarriageReturn();
    void Backspace();
    void Tab();
    void ReverseIndex();

    // Cursor movement
    void SetCursorPos(int row, int col);
    void MoveCursorUp(int n);
    void MoveCursorDown(int n);
    void MoveCursorForward(int n);
    void MoveCursorBack(int n);
    void SetCursorCol(int col);
    void SetCursorRow(int row);
    void SaveCursor();
    void RestoreCursor();

    // Erase
    void EraseDisplay(int mode);
    void EraseLine(int mode);
    void EraseChars(int n);

    // Line operations
    void InsertLines(int n);
    void DeleteLines(int n);
    void DeleteChars(int n);
    void InsertChars(int n);

    // Scrolling
    void ScrollUp(int n);
    void ScrollDown(int n);
    void SetScrollRegion(int top, int bottom);

    // Scrollback
    const Cell& ViewAt(int viewRow, int col) const;
    const Cell& CellAtDocumentRow(int documentRow, int col) const;
    int ViewRowToDocumentRow(int viewRow) const;
    int DocumentRowToViewRow(int documentRow) const;
    void ScrollBack(int lines);
    void ScrollForward(int lines);
    void ScrollToBottom();
    int GetScrollOffset() const { return m_scrollOffset; }
    int GetScrollbackSize() const { return static_cast<int>(m_scrollback.size()); }
    int GetDocumentRowCount() const { return static_cast<int>(m_scrollback.size()) + m_rows; }

    // Attributes
    void ResetAttributes();
    void SetBold(bool v);
    void SetItalic(bool v);
    void SetUnderline(bool v);
    void SetInverse(bool v);
    void SetFg(uint8_t idx);
    void SetBg(uint8_t idx);
    void SetFgRgb(uint8_t r, uint8_t g, uint8_t b);
    void SetBgRgb(uint8_t r, uint8_t g, uint8_t b);
    void SetDefaultFg();
    void SetDefaultBg();

    // Modes
    void SetMode(int mode, bool enabled);

private:
    void ClearLine(int row);
    void ClearCell(Cell& cell);
    void ClampCursor();

    std::vector<Cell> m_cells;
    int m_cols = 0;
    int m_rows = 0;
    int m_cursorRow = 0;
    int m_cursorCol = 0;
    bool m_wrapPending = false;

    Cell m_attr;  // current attribute template

    int m_scrollTop = 0;
    int m_scrollBottom = 0;

    bool m_cursorVisible = true;
    bool m_appCursorKeys = false;
    bool m_bracketedPaste = false;
    std::string m_title;

    // Saved cursor
    int m_savedCursorRow = 0;
    int m_savedCursorCol = 0;
    Cell m_savedAttr;

    // Scrollback buffer
    std::deque<std::vector<Cell>> m_scrollback;
    int m_scrollOffset = 0;
    int m_maxScrollback = 10000;

    // Alternate screen buffer
    bool m_altScreenActive = false;
    std::vector<Cell> m_savedMainBuffer;
    int m_savedMainCursorRow = 0;
    int m_savedMainCursorCol = 0;
    Cell m_savedMainAttr;
};
