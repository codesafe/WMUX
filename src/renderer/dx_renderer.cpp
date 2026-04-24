#include "renderer/dx_renderer.h"
#include "url_detect.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {
const wchar_t* const kHelpLines[] = {
    L"wmux help",
    L"",
    L"=== Prefix commands (after Ctrl+B) ===",
    L"",
    L"  % or v          split vertical (left/right)",
    L"  \" or h          split horizontal (top/bottom)",
    L"  x               close current pane",
    L"  z               toggle zoom",
    L"  o               open settings",
    L"  arrows          move pane focus",
    L"  Ctrl+B          send literal Ctrl+B",
    L"",
    L"=== Direct shortcuts ===",
    L"",
    L"  Ctrl+arrows     move pane focus",
    L"  Alt+H           toggle help popup",
    L"  Alt+arrows      move pane layout position",
    L"  Alt+Shift+arrows swap pane contents",
    L"  Shift+arrows    expand text selection",
    L"  Ctrl+A          select all",
    L"  Ctrl+C          copy selection or send SIGINT",
    L"  Ctrl+V          paste",
    L"  Ctrl+Shift+D    detach current pane to a new wmux window",
    L"  Ctrl+Shift+R    open a new empty wmux window",
    L"  Shift+PageUp    scrollback up",
    L"  Shift+PageDown  scrollback down",
    L"",
    L"=== Mouse ===",
    L"",
    L"  left drag        select text and activate pane",
    L"  Shift+left drag  move pane (within or between windows)",
    L"  drop preview     split top/right/bottom/left or swap",
    L"  double click     select word (open URL if on link)",
    L"  right click      copy selection or paste",
    L"  mouse wheel      scrollback on hovered pane",
    L"  scrollbar drag   adjust scroll position",
    L"  separator drag   resize pane ratio",
    L"",
    L"=== Settings ===",
    L"",
    L"  background color and separator color are configurable",
    L"  dim inactive panes and prefix overlay can be toggled",
    L"  idle effect and wheel scroll lines are configurable",
    L"",
    L"=== Separate process ===",
    L"",
    L"  Ctrl+Shift+D     detach pane to independent process window",
    L"  Shift+drag       drag pane back to merge between windows",
    L"  detached pane    keeps running even if original window closes",
    L"  reconnect        Shift+drag from detached window to any wmux",
    L"",
    L"=== Misc ===",
    L"",
    L"  folder drop      create a pane on the right side",
    L"  Backspace       sends 0x7F (DEL)",
    L"  ESC             close help",
};

constexpr int kHelpLineCount = static_cast<int>(sizeof(kHelpLines) / sizeof(kHelpLines[0]));
}

bool DxRenderer::Initialize(HWND hwnd, const std::wstring& fontName, float fontSize, uint32_t bgColor) {
    m_hwnd = hwnd;
    m_fontName = fontName;
    m_fontSize = fontSize;
    m_dpi = GetDpiForWindow(hwnd);
    if (m_dpi == 0)
        m_dpi = 96;
    SetBackgroundColor(bgColor);
    InitPalette();

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   m_pFactory.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(m_pDWriteFactory.GetAddressOf()));
    if (FAILED(hr)) return false;

    if (!RecreateTextFormat()) return false;

    if (!MeasureCellSize()) return false;
    if (!CreateDeviceResources()) return false;

    return true;
}

bool DxRenderer::SetDpi(UINT dpi) {
    if (dpi == 0)
        dpi = 96;
    if (m_dpi == dpi)
        return true;

    m_dpi = dpi;
    if (!RecreateTextFormat())
        return false;
    return MeasureCellSize();
}

bool DxRenderer::UpdateFont(const std::wstring& fontName, float fontSize) {
    m_fontName = fontName;
    m_fontSize = fontSize;

    if (!RecreateTextFormat())
        return false;
    return MeasureCellSize();
}

bool DxRenderer::RecreateTextFormat() {
    if (!m_pDWriteFactory)
        return false;

    m_pTextFormat.Reset();
    m_pItalicTextFormat.Reset();
    const float scaledFontSize = m_fontSize * (static_cast<float>(m_dpi) / 96.0f);
    HRESULT hr = m_pDWriteFactory->CreateTextFormat(
        m_fontName.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, scaledFontSize, L"en-US",
        m_pTextFormat.GetAddressOf());
    if (FAILED(hr)) return false;

    m_pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    hr = m_pDWriteFactory->CreateTextFormat(
        m_fontName.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_ITALIC,
        DWRITE_FONT_STRETCH_NORMAL, scaledFontSize, L"en-US",
        m_pItalicTextFormat.GetAddressOf());
    if (SUCCEEDED(hr)) {
        m_pItalicTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_pItalicTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        m_pItalicTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    return true;
}

void DxRenderer::SetBackgroundColor(uint32_t rgb) {
    m_defaultBg = D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        1.0f);
}

void DxRenderer::SetSeparatorColor(uint32_t rgb) {
    m_separatorColor = D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        1.0f);
}

int DxRenderer::GetHelpLineCount() {
    return kHelpLineCount;
}

bool DxRenderer::MeasureCellSize() {
    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = m_pDWriteFactory->CreateTextLayout(
        L"M", 1, m_pTextFormat.Get(), 10000.0f, 10000.0f,
        layout.GetAddressOf());
    if (FAILED(hr)) return false;

    DWRITE_TEXT_METRICS textMetrics;
    layout->GetMetrics(&textMetrics);
    m_cellWidth = textMetrics.widthIncludingTrailingWhitespace;

    DWRITE_LINE_METRICS lineMetrics;
    UINT32 lineCount;
    hr = layout->GetLineMetrics(&lineMetrics, 1, &lineCount);
    if (FAILED(hr)) return false;

    m_cellHeight = lineMetrics.height;
    m_baseline = lineMetrics.baseline;

    return true;
}

float DxRenderer::MeasureTextWidth(const std::wstring& text) const {
    if (!m_pDWriteFactory || !m_pTextFormat || text.empty())
        return 0.0f;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = m_pDWriteFactory->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()), m_pTextFormat.Get(),
        4096.0f, m_cellHeight + 8.0f, layout.GetAddressOf());
    if (FAILED(hr))
        return 0.0f;

    DWRITE_TEXT_METRICS metrics = {};
    hr = layout->GetMetrics(&metrics);
    if (FAILED(hr))
        return 0.0f;
    return metrics.widthIncludingTrailingWhitespace;
}

bool DxRenderer::CreateDeviceResources() {
    if (m_pRenderTarget) return true;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    m_width = rc.right - rc.left;
    m_height = rc.bottom - rc.top;

    D2D1_SIZE_U size = D2D1::SizeU(m_width, m_height);
    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties();
    properties.dpiX = 96.0f;
    properties.dpiY = 96.0f;
    HRESULT hr = m_pFactory->CreateHwndRenderTarget(
        properties,
        D2D1::HwndRenderTargetProperties(m_hwnd, size),
        m_pRenderTarget.GetAddressOf());
    if (FAILED(hr)) return false;

    m_pRenderTarget->SetDpi(96.0f, 96.0f);
    m_pRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    hr = m_pRenderTarget->CreateSolidColorBrush(
        D2D1::ColorF(D2D1::ColorF::White), m_pBrush.GetAddressOf());
    return SUCCEEDED(hr);
}

void DxRenderer::DiscardDeviceResources() {
    m_pBrush.Reset();
    m_pRenderTarget.Reset();
}

void DxRenderer::Resize(UINT width, UINT height) {
    m_width = width;
    m_height = height;
    if (m_pRenderTarget) {
        HRESULT hr = m_pRenderTarget->Resize(D2D1::SizeU(width, height));
        if (FAILED(hr))
            DiscardDeviceResources();
    }
    m_matrixInitialized = false;
}

int DxRenderer::CalcCols(UINT width) const {
    if (m_cellWidth <= 0) return 80;
    // Account for left + right padding (4px each = 8px total)
    float availableWidth = static_cast<float>(width) - (GetPanePadding() * 2);
    if (availableWidth < m_cellWidth) return 1;
    return (std::max)(1, static_cast<int>(availableWidth / m_cellWidth));
}

int DxRenderer::CalcRows(UINT height) const {
    if (m_cellHeight <= 0) return 24;
    // Account for top + bottom padding (4px each = 8px total)
    float availableHeight = static_cast<float>(height) - (GetPanePadding() * 2);
    if (availableHeight < m_cellHeight) return 1;
    return (std::max)(1, static_cast<int>(availableHeight / m_cellHeight));
}

void DxRenderer::Render(const TerminalBuffer& buffer) {
    if (!CreateDeviceResources()) return;

    m_pRenderTarget->BeginDraw();
    m_pRenderTarget->Clear(m_defaultBg);

    int rows = buffer.GetRows();
    int cols = buffer.GetCols();
    int cursorRow = buffer.GetCursorRow();
    int cursorCol = buffer.GetCursorCol();
    bool cursorVisible = buffer.IsCursorVisible();

    for (int r = 0; r < rows; r++) {
        float y = r * m_cellHeight;
        if (y >= m_height) break;

        for (int c = 0; c < cols; c++) {
            float x = c * m_cellWidth;
            if (x >= m_width) break;

            const Cell& cell = buffer.At(r, c);

            // Skip trailing half of wide characters
            if (cell.width == 0) continue;

            int span = (cell.width == 2) ? 2 : 1;
            float totalW = span * m_cellWidth;

            bool isCursor = cursorVisible && r == cursorRow && c == cursorCol;

            D2D1_COLOR_F fg = GetCellFgColor(cell);
            D2D1_COLOR_F bg = GetCellBgColor(cell);

            if (cell.flags & CELL_INVERSE) std::swap(fg, bg);
            if (isCursor) std::swap(fg, bg);

            D2D1_RECT_F rect = {x, y, x + totalW, y + m_cellHeight};

            // Draw background if non-default or cursor
            bool needBg = !(cell.flags & CELL_BG_DEFAULT) ||
                          (cell.flags & CELL_INVERSE) || isCursor;
            if (needBg) {
                m_pBrush->SetColor(bg);
                m_pRenderTarget->FillRectangle(rect, m_pBrush.Get());
            }

            // Draw underline
            if (cell.flags & CELL_UNDERLINE) {
                m_pBrush->SetColor(fg);
                uint8_t ulStyle = (cell.flags2 & CELL_UL_STYLE_MASK) >> 5;
                float ulY = y + m_cellHeight - 1;
                if (ulStyle == UL_DOUBLE) {
                    m_pRenderTarget->FillRectangle({x, ulY - 2, x + totalW, ulY - 1}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({x, ulY, x + totalW, ulY + 1}, m_pBrush.Get());
                } else if (ulStyle == UL_CURLY) {
                    for (float px = x; px < x + totalW; px += 2.0f) {
                        float wave = (fmodf(px - x, 4.0f) < 2.0f) ? -1.0f : 0.0f;
                        m_pRenderTarget->FillRectangle({px, ulY + wave, px + 1, ulY + wave + 1}, m_pBrush.Get());
                    }
                } else if (ulStyle == UL_DOTTED) {
                    for (float px = x; px < x + totalW; px += 4.0f)
                        m_pRenderTarget->FillRectangle({px, ulY, px + 2, ulY + 1}, m_pBrush.Get());
                } else if (ulStyle == UL_DASHED) {
                    for (float px = x; px < x + totalW; px += 6.0f)
                        m_pRenderTarget->FillRectangle({px, ulY, px + 4, ulY + 1}, m_pBrush.Get());
                } else {
                    m_pRenderTarget->FillRectangle({x, ulY, x + totalW, ulY + 1}, m_pBrush.Get());
                }
            }

            if (cell.flags2 & CELL_STRIKETHROUGH) {
                m_pBrush->SetColor(fg);
                float midY = y + m_cellHeight / 2;
                m_pRenderTarget->FillRectangle({x, midY, x + totalW, midY + 1}, m_pBrush.Get());
            }

            if (cell.flags2 & CELL_OVERLINE) {
                m_pBrush->SetColor(fg);
                m_pRenderTarget->FillRectangle({x, y, x + totalW, y + 1}, m_pBrush.Get());
            }

            // Draw character (skip if concealed)
            if (!(cell.flags2 & CELL_CONCEAL) && cell.ch != L' ' && cell.ch != 0) {
                m_pBrush->SetColor(fg);
                D2D1_RECT_F textRect = {x, y, x + totalW, y + m_cellHeight};
                auto* fmt = (cell.flags & CELL_ITALIC && m_pItalicTextFormat)
                            ? m_pItalicTextFormat.Get() : m_pTextFormat.Get();
                if (cell.ch2 != 0) {
                    wchar_t pair[2] = {cell.ch, cell.ch2};
                    m_pRenderTarget->DrawText(pair, 2, fmt,
                                              textRect, m_pBrush.Get(),
                                              D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                } else {
                    m_pRenderTarget->DrawText(&cell.ch, 1, fmt,
                                              textRect, m_pBrush.Get(),
                                              D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                }
            }
        }
    }

    HRESULT hr = m_pRenderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
}

D2D1_COLOR_F DxRenderer::GetCellFgColor(const Cell& cell) const {
    D2D1_COLOR_F color;
    if (cell.flags & CELL_FG_DEFAULT) {
        color = m_defaultFg;
    } else if (cell.flags & CELL_FG_RGB) {
        color = D2D1::ColorF(
            ((cell.fgRgb >> 16) & 0xFF) / 255.0f,
            ((cell.fgRgb >> 8) & 0xFF) / 255.0f,
            (cell.fgRgb & 0xFF) / 255.0f);
    } else {
        uint8_t idx = cell.fg;
        if ((cell.flags & CELL_BOLD) && idx < 8) idx += 8;
        color = m_palette[idx];
    }
    if (cell.flags2 & CELL_DIM) {
        color.r *= 0.5f;
        color.g *= 0.5f;
        color.b *= 0.5f;
    }
    return color;
}

D2D1_COLOR_F DxRenderer::GetCellBgColor(const Cell& cell) const {
    if (cell.flags & CELL_BG_DEFAULT)
        return m_defaultBg;
    if (cell.flags & CELL_BG_RGB) {
        return D2D1::ColorF(
            ((cell.bgRgb >> 16) & 0xFF) / 255.0f,
            ((cell.bgRgb >> 8) & 0xFF) / 255.0f,
            (cell.bgRgb & 0xFF) / 255.0f);
    }
    return m_palette[cell.bg];
}

bool DxRenderer::BeginFrame() {
    if (!CreateDeviceResources()) return false;
    m_pRenderTarget->BeginDraw();
    m_pRenderTarget->Clear(m_defaultBg);
    return true;
}

void DxRenderer::EndFrame() {
    HRESULT hr = m_pRenderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
}

static bool CellInSelection(int documentRow, int c, const DxRenderer::Selection* sel) {
    if (!sel || !sel->active) return false;
    int sr = sel->startDocumentRow, sc = sel->startCol;
    int er = sel->endDocumentRow, ec = sel->endCol;
    if (sr > er || (sr == er && sc > ec)) {
        std::swap(sr, er); std::swap(sc, ec);
    }
    if (documentRow < sr || documentRow > er) return false;
    if (documentRow == sr && documentRow == er) return c >= sc && c <= ec;
    if (documentRow == sr) return c >= sc;
    if (documentRow == er) return c <= ec;
    return true;
}



void DxRenderer::RenderPane(const TerminalBuffer& buffer, D2D1_RECT_F rect,
                            bool isActive, bool isZoomed, bool scrollbarDragging,
                            const Selection* sel, bool dimInactive,
                            const ImeComposition* ime) {
    m_pRenderTarget->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);

    // Fill pane background
    m_pBrush->SetColor(m_defaultBg);
    m_pRenderTarget->FillRectangle(rect, m_pBrush.Get());

    // Add padding to prevent text from overlapping with border
    const float PANE_PADDING = 4.0f;
    D2D1_RECT_F contentRect = {
        rect.left + PANE_PADDING,
        rect.top + PANE_PADDING,
        rect.right - PANE_PADDING,
        rect.bottom - PANE_PADDING
    };

    int rows = buffer.GetRows();
    int cols = buffer.GetCols();
    int cursorRow = buffer.GetCursorRow();
    int cursorCol = buffer.GetCursorCol();
    bool cursorVisible = buffer.IsCursorVisible();
    int scrollOffset = buffer.GetScrollOffset();
    bool useViewAt = (scrollOffset > 0);


    // Pre-draw selection background as continuous row spans (no sub-pixel gaps)
    if (sel && sel->active) {
        m_pBrush->SetColor(D2D1::ColorF(0.2f, 0.4f, 0.8f, 1.0f));
        int sr = sel->startDocumentRow, sc = sel->startCol;
        int er = sel->endDocumentRow, ec = sel->endCol;
        if (sr > er || (sr == er && sc > ec)) { std::swap(sr, er); std::swap(sc, ec); }
        for (int r = 0; r < rows; r++) {
            int documentRow = buffer.ViewRowToDocumentRow(r);
            if (documentRow < sr || documentRow > er)
                continue;
            float ry = contentRect.top + r * m_cellHeight;
            if (ry >= contentRect.bottom) break;
            int cS = (documentRow == sr) ? sc : 0;
            int cE = (documentRow == er) ? ec : cols - 1;
            if (cE >= cols) cE = cols - 1;
            float x1 = contentRect.left + cS * m_cellWidth;
            float x2 = contentRect.left + (cE + 1) * m_cellWidth;
            m_pRenderTarget->FillRectangle({x1, ry, x2, ry + m_cellHeight}, m_pBrush.Get());
        }
    }

    // Use aliased mode for crisp cell boundaries
    m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    for (int r = 0; r < rows; r++) {
        // Pixel-snap: integer boundaries eliminate sub-pixel gaps
        float y  = floorf(contentRect.top + r * m_cellHeight);
        float y2 = floorf(contentRect.top + (r + 1) * m_cellHeight);
        if (y >= contentRect.bottom) break;
        int documentRow = buffer.ViewRowToDocumentRow(r);

        // Detect URLs in this row
        auto rowCharAt = [&](int col) -> wchar_t {
            if (col < 0 || col >= cols) return 0;
            const Cell& c2 = useViewAt ? buffer.ViewAt(r, col) : buffer.At(r, col);
            return c2.ch;
        };
        auto urlSpans = DetectUrls(cols, rowCharAt);
        auto isInUrl = [&](int col) -> bool {
            for (auto& u : urlSpans)
                if (col >= u.startCol && col <= u.endCol) return true;
            return false;
        };

        for (int c = 0; c < cols; c++) {
            const Cell& sourceCell = useViewAt ? buffer.ViewAt(r, c) : buffer.At(r, c);
            if (sourceCell.width == 0) continue;

            Cell cell = sourceCell;
            bool scrambled = false;



            int span = (cell.width == 2) ? 2 : 1;
            float x  = floorf(contentRect.left + c * m_cellWidth);
            float x2 = floorf(contentRect.left + (c + span) * m_cellWidth);
            if (x >= contentRect.right) break;

            bool isCursor = !useViewAt && cursorVisible &&
                            r == cursorRow && c == cursorCol;
            bool cellIsUrl = !scrambled && isInUrl(c);

            D2D1_COLOR_F fg = GetCellFgColor(cell);
            D2D1_COLOR_F bg = GetCellBgColor(cell);

            bool isSelected = CellInSelection(documentRow, c, sel);

            if (cell.flags & CELL_INVERSE) std::swap(fg, bg);
            if (isCursor) std::swap(fg, bg);
            if (isSelected) {
                fg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            } else if (cellIsUrl) {
                fg = D2D1::ColorF(0.4f, 0.7f, 1.0f, 1.0f);
            }

            D2D1_RECT_F cellRect = {x, y, x2, y2};

            bool needBg = !isSelected &&
                          (!(cell.flags & CELL_BG_DEFAULT) ||
                           (cell.flags & CELL_INVERSE) || isCursor);
            if (needBg) {
                m_pBrush->SetColor(bg);
                m_pRenderTarget->FillRectangle(cellRect, m_pBrush.Get());
            }

            if (cell.flags & CELL_UNDERLINE || cellIsUrl) {
                m_pBrush->SetColor(fg);
                uint8_t ulStyle = (cell.flags2 & CELL_UL_STYLE_MASK) >> 5;
                float ulY = y2 - 1;
                if (!cellIsUrl && ulStyle == UL_DOUBLE) {
                    m_pRenderTarget->FillRectangle({x, ulY - 2, x2, ulY - 1}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({x, ulY, x2, ulY + 1}, m_pBrush.Get());
                } else if (!cellIsUrl && ulStyle == UL_CURLY) {
                    for (float px = x; px < x2; px += 2.0f) {
                        float wave = (fmodf(px - x, 4.0f) < 2.0f) ? -1.0f : 0.0f;
                        m_pRenderTarget->FillRectangle({px, ulY + wave, px + 1, ulY + wave + 1}, m_pBrush.Get());
                    }
                } else if (!cellIsUrl && ulStyle == UL_DOTTED) {
                    for (float px = x; px < x2; px += 4.0f)
                        m_pRenderTarget->FillRectangle({px, ulY, px + 2, ulY + 1}, m_pBrush.Get());
                } else if (!cellIsUrl && ulStyle == UL_DASHED) {
                    for (float px = x; px < x2; px += 6.0f)
                        m_pRenderTarget->FillRectangle({px, ulY, px + 4, ulY + 1}, m_pBrush.Get());
                } else {
                    m_pRenderTarget->FillRectangle({x, ulY, x2, ulY + 1}, m_pBrush.Get());
                }
            }

            if (cell.flags2 & CELL_STRIKETHROUGH) {
                m_pBrush->SetColor(fg);
                float midY = y + (y2 - y) / 2;
                m_pRenderTarget->FillRectangle({x, midY, x2, midY + 1}, m_pBrush.Get());
            }

            if (cell.flags2 & CELL_OVERLINE) {
                m_pBrush->SetColor(fg);
                m_pRenderTarget->FillRectangle({x, y, x2, y + 1}, m_pBrush.Get());
            }

            if (cell.ch == L' ' || cell.ch == 0) continue;

            // Skip drawing if concealed
            if (cell.flags2 & CELL_CONCEAL) continue;

            // Block elements (U+2580-U+259F): render as geometric fills
            wchar_t ch = cell.ch;
            float cw = x2 - x, ch2f = y2 - y;
            if (ch >= 0x2580 && ch <= 0x259F) {
                // Always fill background first (critical for half-block color art)
                m_pBrush->SetColor(bg);
                m_pRenderTarget->FillRectangle(cellRect, m_pBrush.Get());
                m_pBrush->SetColor(fg);
                if (ch == 0x2580) // ▀ upper half
                    m_pRenderTarget->FillRectangle({x, y, x2, y + ch2f / 2}, m_pBrush.Get());
                else if (ch >= 0x2581 && ch <= 0x2587) { // lower 1/8 to 7/8
                    float frac = (ch - 0x2580) / 8.0f;
                    m_pRenderTarget->FillRectangle({x, y + ch2f * (1.0f - frac), x2, y2}, m_pBrush.Get());
                } else if (ch == 0x2588) // █ full block
                    m_pRenderTarget->FillRectangle(cellRect, m_pBrush.Get());
                else if (ch >= 0x2589 && ch <= 0x258F) { // left 7/8 to 1/8
                    float frac = (0x2590 - ch) / 8.0f;
                    m_pRenderTarget->FillRectangle({x, y, x + cw * frac, y2}, m_pBrush.Get());
                } else if (ch == 0x2590) // ▐ right half
                    m_pRenderTarget->FillRectangle({x + cw / 2, y, x2, y2}, m_pBrush.Get());
                else if (ch == 0x2591) { // ░ light shade (25%)
                    m_pBrush->SetColor({fg.r, fg.g, fg.b, 0.25f});
                    m_pRenderTarget->FillRectangle(cellRect, m_pBrush.Get());
                } else if (ch == 0x2592) { // ▒ medium shade (50%)
                    m_pBrush->SetColor({fg.r, fg.g, fg.b, 0.5f});
                    m_pRenderTarget->FillRectangle(cellRect, m_pBrush.Get());
                } else if (ch == 0x2593) { // ▓ dark shade (75%)
                    m_pBrush->SetColor({fg.r, fg.g, fg.b, 0.75f});
                    m_pRenderTarget->FillRectangle(cellRect, m_pBrush.Get());
                } else if (ch == 0x2594) // ▔ upper 1/8
                    m_pRenderTarget->FillRectangle({x, y, x2, y + ch2f / 8}, m_pBrush.Get());
                else if (ch == 0x2595) // ▕ right 1/8
                    m_pRenderTarget->FillRectangle({x2 - cw / 8, y, x2, y2}, m_pBrush.Get());
                else { // quadrant blocks 0x2596-0x259F
                    float mx = x + cw / 2, my = y + ch2f / 2;
                    int q = ch - 0x2596; // 0-9 maps to quadrant combinations
                    // Quadrant bit pattern: bit0=BL, bit1=BR, bit2=TL, bit3=TR
                    // U+2596-259F: ▖▗▘▙▚▛▜▝▞▟
                    static const uint8_t qmap[] = {
                        0x01,0x02,0x04,0x07,0x06,0x0D,0x0E,0x08,0x09,0x0B};
                    if (q >= 0 && q < 10) {
                        uint8_t bits = qmap[q];
                        if (bits & 0x01) m_pRenderTarget->FillRectangle({x, my, mx, y2}, m_pBrush.Get());
                        if (bits & 0x02) m_pRenderTarget->FillRectangle({mx, my, x2, y2}, m_pBrush.Get());
                        if (bits & 0x04) m_pRenderTarget->FillRectangle({x, y, mx, my}, m_pBrush.Get());
                        if (bits & 0x08) m_pRenderTarget->FillRectangle({mx, y, x2, my}, m_pBrush.Get());
                    }
                }
                continue;
            }

            // Box-drawing characters (U+2500-U+257F): render as lines
            if (ch >= 0x2500 && ch <= 0x257F) {
                // Fill background first
                if (!(cell.flags & CELL_BG_DEFAULT) || (cell.flags & CELL_INVERSE)) {
                    m_pBrush->SetColor(bg);
                    m_pRenderTarget->FillRectangle(cellRect, m_pBrush.Get());
                }
                m_pBrush->SetColor(fg);
                // Pixel-snap center and thickness for crisp lines
                float mx = floorf(x + cw / 2);
                float my = floorf(y + ch2f / 2);
                float ht = 0.5f; // half-thickness for 1-pixel lines
                switch (ch) {
                case 0x2500: case 0x2501: // ─ ━
                    if (ch == 0x2501) ht = 1.0f; // thick horizontal: 2px total
                    // Extend slightly to connect with adjacent cells
                    m_pRenderTarget->FillRectangle({x - 0.5f, my - ht, x2 + 0.5f, my + ht}, m_pBrush.Get());
                    break;
                case 0x2502: case 0x2503: // │ ┃
                    if (ch == 0x2503) ht = 1.0f; // thick vertical: 2px total
                    // Extend slightly to connect with adjacent cells
                    m_pRenderTarget->FillRectangle({mx - ht, y - 0.5f, mx + ht, y2 + 0.5f}, m_pBrush.Get());
                    break;
                case 0x250C: case 0x250F: // ┌ ┏ (down, right)
                    m_pRenderTarget->FillRectangle({mx - ht, my - ht, mx + ht, y2 + 0.5f}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({mx - ht, my - ht, x2 + 0.5f, my + ht}, m_pBrush.Get());
                    break;
                case 0x2510: case 0x2513: // ┐ ┓ (down, left)
                    m_pRenderTarget->FillRectangle({mx - ht, my - ht, mx + ht, y2 + 0.5f}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({x - 0.5f, my - ht, mx + ht, my + ht}, m_pBrush.Get());
                    break;
                case 0x2514: case 0x2517: // └ ┗ (up, right)
                    m_pRenderTarget->FillRectangle({mx - ht, y - 0.5f, mx + ht, my + ht}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({mx - ht, my - ht, x2 + 0.5f, my + ht}, m_pBrush.Get());
                    break;
                case 0x2518: case 0x251B: // ┘ ┛ (up, left)
                    m_pRenderTarget->FillRectangle({mx - ht, y - 0.5f, mx + ht, my + ht}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({x - 0.5f, my - ht, mx + ht, my + ht}, m_pBrush.Get());
                    break;
                case 0x251C: case 0x2523: // ├ ┣ (up, down, right)
                    m_pRenderTarget->FillRectangle({mx - ht, y - 0.5f, mx + ht, y2 + 0.5f}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({mx - ht, my - ht, x2 + 0.5f, my + ht}, m_pBrush.Get());
                    break;
                case 0x2524: case 0x252B: // ┤ ┫ (up, down, left)
                    m_pRenderTarget->FillRectangle({mx - ht, y - 0.5f, mx + ht, y2 + 0.5f}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({x - 0.5f, my - ht, mx + ht, my + ht}, m_pBrush.Get());
                    break;
                case 0x252C: case 0x2533: // ┬ ┳ (left, right, down)
                    m_pRenderTarget->FillRectangle({x - 0.5f, my - ht, x2 + 0.5f, my + ht}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({mx - ht, my - ht, mx + ht, y2 + 0.5f}, m_pBrush.Get());
                    break;
                case 0x2534: case 0x253B: // ┴ ┻ (left, right, up)
                    m_pRenderTarget->FillRectangle({x - 0.5f, my - ht, x2 + 0.5f, my + ht}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({mx - ht, y - 0.5f, mx + ht, my + ht}, m_pBrush.Get());
                    break;
                case 0x253C: case 0x254B: // ┼ ╋ (all directions)
                    m_pRenderTarget->FillRectangle({x - 0.5f, my - ht, x2 + 0.5f, my + ht}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({mx - ht, y - 0.5f, mx + ht, y2 + 0.5f}, m_pBrush.Get());
                    break;
                case 0x2550: // ═ double horizontal
                    m_pRenderTarget->FillRectangle({x - 0.5f, my - 1.5f, x2 + 0.5f, my - 0.5f}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({x - 0.5f, my + 0.5f, x2 + 0.5f, my + 1.5f}, m_pBrush.Get());
                    break;
                case 0x2551: // ║ double vertical
                    m_pRenderTarget->FillRectangle({mx - 1.5f, y - 0.5f, mx - 0.5f, y2 + 0.5f}, m_pBrush.Get());
                    m_pRenderTarget->FillRectangle({mx + 0.5f, y - 0.5f, mx + 1.5f, y2 + 0.5f}, m_pBrush.Get());
                    break;
                case 0x256D: case 0x256E: case 0x256F: case 0x2570: {
                    // Rounded corners: ╭ ╮ ╯ ╰
                    float radius = (std::min)(cw, ch2f) * 0.5f;
                    ComPtr<ID2D1PathGeometry> pathGeometry;
                    if (SUCCEEDED(m_pFactory->CreatePathGeometry(pathGeometry.GetAddressOf()))) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(pathGeometry->Open(sink.GetAddressOf()))) {
                            D2D1_POINT_2F p0, p1, p2, p3;
                            if (ch == 0x256D) { // ╭ connects → and ↓
                                p0 = {mx, y2 + 0.5f};
                                p1 = {mx, my + radius};
                                p2 = {mx + radius, my};
                                p3 = {x2 + 0.5f, my};
                            } else if (ch == 0x256E) { // ╮ connects ← and ↓
                                p0 = {x - 0.5f, my};
                                p1 = {mx - radius, my};
                                p2 = {mx, my + radius};
                                p3 = {mx, y2 + 0.5f};
                            } else if (ch == 0x256F) { // ╯ connects ← and ↑
                                p0 = {mx, y - 0.5f};
                                p1 = {mx, my - radius};
                                p2 = {mx - radius, my};
                                p3 = {x - 0.5f, my};
                            } else { // ╰ connects → and ↑
                                p0 = {x2 + 0.5f, my};
                                p1 = {mx + radius, my};
                                p2 = {mx, my - radius};
                                p3 = {mx, y - 0.5f};
                            }
                            sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddLine(p1);
                            sink->AddArc(D2D1::ArcSegment(p2,
                                D2D1::SizeF(radius, radius), 0.0f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                D2D1_ARC_SIZE_SMALL));
                            sink->AddLine(p3);
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                            m_pRenderTarget->DrawGeometry(pathGeometry.Get(), m_pBrush.Get(), 1.0f);
                            m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                        }
                    }
                    break;
                }
                default: // Fallback: draw with font for unhandled box-drawing
                    m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    m_pRenderTarget->DrawText(&cell.ch, 1, m_pTextFormat.Get(),
                                              cellRect, m_pBrush.Get(),
                                              D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                    m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                    break;
                }
                continue;
            }

            // Braille patterns (U+2800-U+28FF): render as dot matrix
            if (ch >= 0x2800 && ch <= 0x28FF) {
                // Fill background first
                if (!(cell.flags & CELL_BG_DEFAULT) || (cell.flags & CELL_INVERSE)) {
                    m_pBrush->SetColor(bg);
                    m_pRenderTarget->FillRectangle(cellRect, m_pBrush.Get());
                }
                m_pBrush->SetColor(fg);
                int pattern = ch - 0x2800;
                // Braille: 2 columns x 4 rows of dots
                // Bit layout: 0=TL,1=ML,2=BL,3=TR,4=MR,5=BR,6=BLL,7=BLR
                // Standard: col0 bits 0,1,2,6  col1 bits 3,4,5,7
                float dotW = cw / 5.0f;
                float dotH = ch2f / 9.0f;
                float r0 = dotH;
                float cx0 = x + cw * 0.3f, cx1 = x + cw * 0.7f;
                float ry[4] = {y + r0, y + r0 * 3, y + r0 * 5, y + r0 * 7};
                // col 0: bits 0,1,2,6
                if (pattern & 0x01) m_pRenderTarget->FillRectangle({cx0 - dotW, ry[0] - dotH, cx0 + dotW, ry[0] + dotH}, m_pBrush.Get());
                if (pattern & 0x02) m_pRenderTarget->FillRectangle({cx0 - dotW, ry[1] - dotH, cx0 + dotW, ry[1] + dotH}, m_pBrush.Get());
                if (pattern & 0x04) m_pRenderTarget->FillRectangle({cx0 - dotW, ry[2] - dotH, cx0 + dotW, ry[2] + dotH}, m_pBrush.Get());
                if (pattern & 0x40) m_pRenderTarget->FillRectangle({cx0 - dotW, ry[3] - dotH, cx0 + dotW, ry[3] + dotH}, m_pBrush.Get());
                // col 1: bits 3,4,5,7
                if (pattern & 0x08) m_pRenderTarget->FillRectangle({cx1 - dotW, ry[0] - dotH, cx1 + dotW, ry[0] + dotH}, m_pBrush.Get());
                if (pattern & 0x10) m_pRenderTarget->FillRectangle({cx1 - dotW, ry[1] - dotH, cx1 + dotW, ry[1] + dotH}, m_pBrush.Get());
                if (pattern & 0x20) m_pRenderTarget->FillRectangle({cx1 - dotW, ry[2] - dotH, cx1 + dotW, ry[2] + dotH}, m_pBrush.Get());
                if (pattern & 0x80) m_pRenderTarget->FillRectangle({cx1 - dotW, ry[3] - dotH, cx1 + dotW, ry[3] + dotH}, m_pBrush.Get());
                continue;
            }

            // Regular text
            m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            m_pBrush->SetColor(fg);
            auto* fmt = (cell.flags & CELL_ITALIC && m_pItalicTextFormat)
                        ? m_pItalicTextFormat.Get() : m_pTextFormat.Get();
            if (cell.ch2 != 0) {
                wchar_t pair[2] = {cell.ch, cell.ch2};
                m_pRenderTarget->DrawText(pair, 2, fmt,
                                          cellRect, m_pBrush.Get(),
                                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            } else {
                m_pRenderTarget->DrawText(&cell.ch, 1, fmt,
                                          cellRect, m_pBrush.Get(),
                                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }
            m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        }
    }

    // IME composition floating box
    if (ime && ime->composing && !ime->compStr.empty()) {
        int imeWidth = 0;
        for (wchar_t wc : ime->compStr) {
            imeWidth += (wc >= 0x1100 && wc <= 0x115F) || (wc >= 0xAC00 && wc <= 0xD7A3) ||
                        (wc >= 0x2E80 && wc <= 0x9FFF) || (wc >= 0xF900 && wc <= 0xFAFF) ? 2 : 1;
        }

        float padH = 6.0f, padW = 10.0f;
        float iw = imeWidth * m_cellWidth + padW * 2;
        float ih = m_cellHeight + padH * 2;

        float ix = contentRect.left + cursorCol * m_cellWidth;
        float iy = contentRect.top + (cursorRow + 1) * m_cellHeight + 2.0f;

        if (iy + ih > contentRect.bottom)
            iy = contentRect.top + cursorRow * m_cellHeight - ih - 2.0f;
        if (ix + iw > contentRect.right)
            ix = contentRect.right - iw;
        if (ix < contentRect.left)
            ix = contentRect.left;
        if (iy < contentRect.top)
            iy = contentRect.top;

        D2D1_RECT_F boxRect = {ix, iy, ix + iw, iy + ih};

        m_pBrush->SetColor(D2D1::ColorF(0.06f, 0.06f, 0.12f, 0.95f));
        m_pRenderTarget->FillRectangle(boxRect, m_pBrush.Get());

        m_pBrush->SetColor(D2D1::ColorF(0x16C60C, 0.8f));
        m_pRenderTarget->DrawRectangle(boxRect, m_pBrush.Get(), 1.5f);

        D2D1_RECT_F textRect = {ix + padW, iy + padH, ix + iw - padW, iy + ih - padH};
        m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_pBrush->SetColor(D2D1::ColorF(0xE8E8E8));
        m_pRenderTarget->DrawText(ime->compStr.c_str(), static_cast<UINT32>(ime->compStr.size()),
                                  m_pTextFormat.Get(), textRect, m_pBrush.Get(),
                                  D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

        m_pBrush->SetColor(D2D1::ColorF(0x16C60C));
        m_pRenderTarget->FillRectangle({ix + padW, iy + ih - padH - 2, ix + iw - padW, iy + ih - padH}, m_pBrush.Get());
    }

    // Custom scrollbar overlay
    int sbSize = buffer.GetScrollbackSize();
    if (sbSize > 0) {
        int totalLines = sbSize + rows;
        float paneH = contentRect.bottom - contentRect.top;
        float barW = 12.0f;
        float barX = contentRect.right - barW;

        // Track background (semi-transparent)
        m_pBrush->SetColor({0.2f, 0.2f, 0.2f, 0.3f});
        m_pRenderTarget->FillRectangle({barX, contentRect.top, contentRect.right, contentRect.bottom}, m_pBrush.Get());

        // Thumb
        float thumbRatio = static_cast<float>(rows) / totalLines;
        float thumbH = (std::max)(20.0f, paneH * thumbRatio);
        float scrollRange = paneH - thumbH;
        float scrollPos = (sbSize > 0)
            ? (1.0f - static_cast<float>(scrollOffset) / sbSize) * scrollRange
            : scrollRange;

        if (scrollbarDragging)
            m_pBrush->SetColor({0.2f, 0.4f, 0.8f, 0.9f}); // bright blue while dragging
        else
            m_pBrush->SetColor({0.7f, 0.7f, 0.7f, 0.6f});
        m_pRenderTarget->FillRectangle(
            {barX + 1, contentRect.top + scrollPos, contentRect.right - 1, contentRect.top + scrollPos + thumbH},
            m_pBrush.Get());
    }

    // Active pane border highlight (green)
    if (isActive && !isZoomed) {
        m_pBrush->SetColor(D2D1::ColorF(0x16C60C));
        float t = 2.0f;
        m_pRenderTarget->FillRectangle({rect.left, rect.top, rect.right, rect.top + t}, m_pBrush.Get());
        m_pRenderTarget->FillRectangle({rect.left, rect.bottom - t, rect.right, rect.bottom}, m_pBrush.Get());
        m_pRenderTarget->FillRectangle({rect.left, rect.top, rect.left + t, rect.bottom}, m_pBrush.Get());
        m_pRenderTarget->FillRectangle({rect.right - t, rect.top, rect.right, rect.bottom}, m_pBrush.Get());
    }

    // Dim inactive panes with semi-transparent overlay
    if (!isActive && dimInactive) {
        m_pBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f));  // 35% black overlay
        m_pRenderTarget->FillRectangle(rect, m_pBrush.Get());
    }

    m_pRenderTarget->PopAxisAlignedClip();
}

void DxRenderer::RenderSeparator(float x1, float y1, float x2, float y2) {
    m_pBrush->SetColor(m_separatorColor);
    m_pRenderTarget->FillRectangle({x1, y1, x2, y2}, m_pBrush.Get());
}

void DxRenderer::RenderStatusBar(float y, float width, const std::wstring& leftText,
                                 const std::wstring& rightText, bool isZoomed) {
    float barH = m_cellHeight + 4.0f;
    D2D1_RECT_F barRect = {0, y, width, y + barH};

    // Background (orange when zoomed, dark blue otherwise)
    if (isZoomed) {
        m_pBrush->SetColor(D2D1::ColorF(0xFF8C00, 0.7f)); // Dark orange with alpha
    } else {
        m_pBrush->SetColor(D2D1::ColorF(0x1A1A2E));
    }
    m_pRenderTarget->FillRectangle(barRect, m_pBrush.Get());

    // Top border line (brighter orange when zoomed)
    if (isZoomed) {
        m_pBrush->SetColor(D2D1::ColorF(0xFFA500)); // Bright orange
    } else {
        m_pBrush->SetColor(D2D1::ColorF(0x404060));
    }
    m_pRenderTarget->FillRectangle({0, y, width, y + 1.0f}, m_pBrush.Get());

    const float padding = 6.0f;
    const float gap = 12.0f;
    float rightWidth = MeasureTextWidth(rightText);
    float rightStart = width - padding - rightWidth;
    if (rightStart < width * 0.55f)
        rightStart = width * 0.55f;

    if (!leftText.empty()) {
        m_pBrush->SetColor(D2D1::ColorF(0xCCCCCC));
        float leftRight = rightText.empty() ? (width - padding) : (rightStart - gap);
        D2D1_RECT_F textRect = {padding, y + 2.0f, leftRight, y + barH};
        m_pRenderTarget->DrawText(leftText.c_str(), static_cast<UINT32>(leftText.size()),
                                  m_pTextFormat.Get(), textRect, m_pBrush.Get(),
                                  D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    if (!rightText.empty()) {
        m_pBrush->SetColor(D2D1::ColorF(0xF9F1A5));
        D2D1_RECT_F textRect = {rightStart, y + 2.0f, width - padding, y + barH};
        m_pRenderTarget->DrawText(rightText.c_str(), static_cast<UINT32>(rightText.size()),
                                  m_pTextFormat.Get(), textRect, m_pBrush.Get(),
                                  D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
}

void DxRenderer::RenderPrefixIndicator() {
    m_pBrush->SetColor(D2D1::ColorF(0xF9F1A5));
    D2D1_RECT_F indicator = {
        static_cast<float>(m_width) - 14.0f, 4.0f,
        static_cast<float>(m_width) - 4.0f, 14.0f
    };
    m_pRenderTarget->FillRectangle(indicator, m_pBrush.Get());
}

void DxRenderer::RenderPrefixOverlay(const std::wstring& text) {
    if (text.empty())
        return;

    float overlayW = MeasureTextWidth(text) + 20.0f;
    float overlayH = m_cellHeight + 12.0f;
    if (overlayW < 240.0f) overlayW = 240.0f;
    if (overlayW > static_cast<float>(m_width) - 20.0f)
        overlayW = static_cast<float>(m_width) - 20.0f;

    float left = (static_cast<float>(m_width) - overlayW) * 0.5f;
    float top = 8.0f;
    D2D1_RECT_F rect = {left, top, left + overlayW, top + overlayH};

    m_pBrush->SetColor(D2D1::ColorF(0.06f, 0.08f, 0.14f, 0.92f));
    m_pRenderTarget->FillRectangle(rect, m_pBrush.Get());
    m_pBrush->SetColor(D2D1::ColorF(0xF9F1A5));
    m_pRenderTarget->FillRectangle({rect.left, rect.bottom - 2.0f, rect.right, rect.bottom}, m_pBrush.Get());
    m_pBrush->SetColor(D2D1::ColorF(0xE8E8E8));
    D2D1_RECT_F textRect = {rect.left + 10.0f, rect.top + 4.0f, rect.right - 10.0f, rect.bottom};
    m_pRenderTarget->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
                              m_pTextFormat.Get(), textRect, m_pBrush.Get(),
                              D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void DxRenderer::RenderZoomBorder(float width, float height) {
    // Draw thick orange border around entire pane area
    const float borderWidth = 3.0f;
    m_pBrush->SetColor(D2D1::ColorF(0xFFA500)); // Bright orange

    // Top
    m_pRenderTarget->FillRectangle({0, 0, width, borderWidth}, m_pBrush.Get());
    // Bottom
    m_pRenderTarget->FillRectangle({0, height - borderWidth, width, height}, m_pBrush.Get());
    // Left
    m_pRenderTarget->FillRectangle({0, 0, borderWidth, height}, m_pBrush.Get());
    // Right
    m_pRenderTarget->FillRectangle({width - borderWidth, 0, width, height}, m_pBrush.Get());
}

void DxRenderer::RenderHelpPopup(int scrollOffset) {
    float lineHeight = m_cellHeight;
    float visibleHeight = lineHeight * HELP_VISIBLE_LINES;
    float popupHeight = visibleHeight + HELP_POPUP_PADDING * 2.0f;

    float left = (static_cast<float>(m_width) - HELP_POPUP_WIDTH) * 0.5f;
    float top = (static_cast<float>(m_height) - popupHeight) * 0.5f;
    D2D1_RECT_F popupRect = {left, top, left + HELP_POPUP_WIDTH, top + popupHeight};

    // Semi-transparent background
    m_pBrush->SetColor(D2D1::ColorF(0.05f, 0.05f, 0.10f, 0.95f));
    m_pRenderTarget->FillRectangle(popupRect, m_pBrush.Get());

    // Green border
    m_pBrush->SetColor(D2D1::ColorF(0x16C60C));
    m_pRenderTarget->DrawRectangle(popupRect, m_pBrush.Get(), 2.0f);

    // Scrollable content area
    D2D1_RECT_F contentRect = {
        popupRect.left + HELP_POPUP_PADDING,
        popupRect.top + HELP_POPUP_PADDING,
        popupRect.right - HELP_POPUP_PADDING,
        popupRect.bottom - HELP_POPUP_PADDING
    };

    m_pRenderTarget->PushAxisAlignedClip(contentRect, D2D1_ANTIALIAS_MODE_ALIASED);

    // Render help lines with scroll offset
    float y = contentRect.top - (scrollOffset * lineHeight);
    for (int i = 0; i < kHelpLineCount; i++) {
        if (y + lineHeight < contentRect.top) {
            y += lineHeight;
            continue;  // Skip lines above visible area
        }
        if (y > contentRect.bottom) {
            break;  // Stop rendering below visible area
        }

        // Color based on line type
        if (wcsstr(kHelpLines[i], L"\u2501\u2501\u2501") != nullptr) {
            // Section header
            m_pBrush->SetColor(D2D1::ColorF(0xF9F1A5));  // Yellow
        } else if (wcslen(kHelpLines[i]) == 0) {
            // Empty line - skip
            y += lineHeight;
            continue;
        } else if (wcsstr(kHelpLines[i], L"wmux") != nullptr) {
            // Title
            m_pBrush->SetColor(D2D1::ColorF(0x16C60C));  // Green
        } else if (wcsstr(kHelpLines[i], L"ESC") != nullptr && wcsstr(kHelpLines[i], L"close") != nullptr) {
            // Close instruction
            m_pBrush->SetColor(D2D1::ColorF(0xFF6B6B));  // Red
        } else {
            // Normal text
            m_pBrush->SetColor(D2D1::ColorF(0xE8E8E8));  // Light gray
        }

        D2D1_RECT_F textRect = {contentRect.left, y, contentRect.right, y + lineHeight};
        m_pRenderTarget->DrawText(
            kHelpLines[i],
            static_cast<UINT32>(wcslen(kHelpLines[i])),
            m_pTextFormat.Get(),
            textRect,
            m_pBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
        );

        y += lineHeight;
    }

    m_pRenderTarget->PopAxisAlignedClip();

    // Scrollbar
    if (kHelpLineCount * lineHeight > visibleHeight) {
        float scrollbarLeft = popupRect.right - HELP_POPUP_PADDING - HELP_SCROLLBAR_WIDTH;
        float scrollbarTop = contentRect.top;

        // Track
        m_pBrush->SetColor(D2D1::ColorF(0.2f, 0.2f, 0.2f, 0.5f));
        D2D1_RECT_F trackRect = {scrollbarLeft, scrollbarTop,
                                  scrollbarLeft + HELP_SCROLLBAR_WIDTH,
                                  scrollbarTop + visibleHeight};
        m_pRenderTarget->FillRectangle(trackRect, m_pBrush.Get());

        // Thumb
        float contentHeight = kHelpLineCount * lineHeight;
        float thumbHeight = (visibleHeight / contentHeight) * visibleHeight;
        if (thumbHeight < 20.0f) thumbHeight = 20.0f;
        float maxScroll = static_cast<float>(kHelpLineCount - HELP_VISIBLE_LINES);
        if (maxScroll < 1.0f) maxScroll = 1.0f;
        float thumbOffset = (scrollOffset / maxScroll) * (visibleHeight - thumbHeight);

        m_pBrush->SetColor(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.8f));
        D2D1_RECT_F thumbRect = {scrollbarLeft, scrollbarTop + thumbOffset,
                                  scrollbarLeft + HELP_SCROLLBAR_WIDTH,
                                  scrollbarTop + thumbOffset + thumbHeight};
        m_pRenderTarget->FillRectangle(thumbRect, m_pBrush.Get());
    }
}

void DxRenderer::RenderDropZone(D2D1_RECT_F rect, int zone) {
    // zone: 0=top, 1=right, 2=bottom, 3=left, 4=center
    D2D1_RECT_F highlightRect = rect;

    if (zone == 0) {
        // Top half
        highlightRect.bottom = rect.top + (rect.bottom - rect.top) * 0.5f;
    } else if (zone == 1) {
        // Right half
        highlightRect.left = rect.left + (rect.right - rect.left) * 0.5f;
    } else if (zone == 2) {
        // Bottom half
        highlightRect.top = rect.top + (rect.bottom - rect.top) * 0.5f;
    } else if (zone == 3) {
        // Left half
        highlightRect.right = rect.left + (rect.right - rect.left) * 0.5f;
    }
    // zone 4 (center) uses full rect

    // Semi-transparent green overlay
    m_pBrush->SetColor(D2D1::ColorF(0x16C60C, 0.3f));
    m_pRenderTarget->FillRectangle(highlightRect, m_pBrush.Get());

    // Border
    m_pBrush->SetColor(D2D1::ColorF(0x16C60C, 0.8f));
    m_pRenderTarget->DrawRectangle(highlightRect, m_pBrush.Get(), 3.0f);
}

static uint32_t MatrixRand(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

wchar_t DxRenderer::RandomMatrixChar() {
    static const wchar_t pool[] =
        L"ｦｧｨｩｪｫｬｭｮｯ"
        L"ｰｱｲｳｴｵｶｷｸｹ"
        L"ｺｻｼｽｾｿﾀﾁﾂﾃ"
        L"ﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍ"
        L"ﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗ"
        L"ﾘﾙﾚﾛﾜﾝ"
        L"0123456789"
        L"ZSTNRMAHKE";
    constexpr int poolSize = sizeof(pool) / sizeof(wchar_t) - 1;
    return pool[MatrixRand(m_matrixRng) % poolSize];
}

void DxRenderer::SpawnStream(int column, int layer) {
    MatrixStream s;
    s.column = column;
    s.layer = layer;
    s.active = true;
    s.cooldownRemaining = 0;
    s.y = -(float)(MatrixRand(m_matrixRng) % 5) * m_cellHeight;

    float r1 = (float)(MatrixRand(m_matrixRng) % 1000) / 1000.0f;
    float r2 = (float)(MatrixRand(m_matrixRng) % 1000) / 1000.0f;
    switch (layer) {
        case 0:
            s.speed = 0.5f + r1 * 1.0f;
            s.length = 15 + (int)(r2 * 26);
            s.columnBrightness = 0.3f + (float)(MatrixRand(m_matrixRng) % 1000) / 5000.0f;
            break;
        case 1:
            s.speed = 1.5f + r1 * 1.5f;
            s.length = 8 + (int)(r2 * 18);
            s.columnBrightness = 0.5f + (float)(MatrixRand(m_matrixRng) % 1000) / 3333.0f;
            break;
        default:
            s.speed = 3.0f + r1 * 3.0f;
            s.length = 5 + (int)(r2 * 11);
            s.columnBrightness = 0.8f + (float)(MatrixRand(m_matrixRng) % 1000) / 5000.0f;
            break;
    }

    s.chars.resize(s.length);
    for (int j = 0; j < s.length; ++j)
        s.chars[j] = RandomMatrixChar();

    m_matrixStreams.push_back(std::move(s));
}

void DxRenderer::RenderMatrixEffect(uint32_t /*frame*/) {
    if (!m_matrixInitialized) {
        InitializeMatrixColumns();
    }

    int curCols = static_cast<int>(m_width / m_cellWidth);
    int curRows = static_cast<int>(m_height / m_cellHeight) + 2;
    if (curCols != m_matrixNumCols || curRows != m_matrixNumRows) {
        m_matrixInitialized = false;
        InitializeMatrixColumns();
    }

    UpdateMatrixColumns();

    m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Pass 1: residue glow
    for (int col = 0; col < m_matrixNumCols; ++col) {
        float x = col * m_cellWidth;
        for (int row = 0; row < m_matrixNumRows; ++row) {
            auto& rc = m_residueGrid[col][row];
            if (rc.brightness <= 0.0f) continue;
            float y = row * m_cellHeight;
            if (y > (float)m_height) break;

            float glowW = m_cellWidth * 0.6f;
            float glowH = m_cellHeight * 0.6f;
            float cx = x + m_cellWidth * 0.5f;
            float cy = y + m_cellHeight * 0.5f;
            m_pBrush->SetColor({0.0f, 0.18f * rc.brightness, 0.0f, 0.5f});
            m_pRenderTarget->FillRectangle(
                {cx - glowW / 2, cy - glowH / 2, cx + glowW / 2, cy + glowH / 2},
                m_pBrush.Get());
        }
    }

    // Pass 2: streams by layer (background -> foreground)
    for (int layerPass = 0; layerPass <= 2; ++layerPass) {
        for (const auto& stream : m_matrixStreams) {
            if (!stream.active || stream.layer != layerPass) continue;

            float x = stream.column * m_cellWidth;
            int charCount = static_cast<int>(stream.chars.size());

            for (int i = 0; i < charCount; ++i) {
                float charY = stream.y - i * m_cellHeight;
                if (charY > (float)m_height) continue;
                if (charY < -m_cellHeight) break;

                float rawB = expf(-3.5f * static_cast<float>(i) / stream.length);
                float b = rawB * stream.columnBrightness;
                if (b < 0.02f) break;

                D2D1_COLOR_F color;
                if (i == 0) {
                    float w = 0.6f + 0.4f * stream.columnBrightness;
                    color = {w * 0.9f, 1.0f, w * 0.9f, 1.0f};
                } else if (i <= 2) {
                    float t = static_cast<float>(i) / 3.0f;
                    float green = 0.95f * b;
                    float rb = (1.0f - t) * 0.7f * b;
                    color = {rb, green, rb, 1.0f};
                } else {
                    color = {0.0f, 0.8f * b, 0.0f, 1.0f};
                }

                D2D1_RECT_F rect = {x, charY, x + m_cellWidth, charY + m_cellHeight};
                m_pBrush->SetColor(color);
                m_pRenderTarget->DrawText(&stream.chars[i], 1, m_pTextFormat.Get(),
                                          rect, m_pBrush.Get(),
                                          D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
    }
}


void DxRenderer::InitializeMatrixColumns() {
    if (m_cellWidth <= 0 || m_cellHeight <= 0) return;

    m_matrixNumCols = static_cast<int>(m_width / m_cellWidth);
    m_matrixNumRows = static_cast<int>(m_height / m_cellHeight) + 2;

    m_matrixStreams.clear();
    m_matrixStreams.reserve(512);

    m_residueGrid.assign(m_matrixNumCols, std::vector<ResidueCell>(m_matrixNumRows));

    m_matrixRng = static_cast<uint32_t>(GetTickCount64() ^ 0xDEADBEEF);
    if (m_matrixRng == 0) m_matrixRng = 1;

    for (int col = 0; col < m_matrixNumCols; ++col) {
        if (MatrixRand(m_matrixRng) % 100 < 40) {
            uint32_t lr = MatrixRand(m_matrixRng) % 100;
            int layer = (lr < 40) ? 0 : (lr < 75) ? 1 : 2;
            SpawnStream(col, layer);
            m_matrixStreams.back().y = static_cast<float>(MatrixRand(m_matrixRng) % m_height);
        }
    }

    m_matrixInitialized = true;
}

void DxRenderer::UpdateMatrixColumns() {
    for (size_t idx = 0; idx < m_matrixStreams.size(); ++idx) {
        auto& stream = m_matrixStreams[idx];

        if (!stream.active) {
            stream.cooldownRemaining -= 1.0f;
            if (stream.cooldownRemaining <= 0) {
                int col = stream.column;
                uint32_t lr = MatrixRand(m_matrixRng) % 100;
                int layer = (lr < 40) ? 0 : (lr < 75) ? 1 : 2;

                MatrixStream s;
                s.column = col;
                s.layer = layer;
                s.active = true;
                s.cooldownRemaining = 0;
                s.y = -(float)(MatrixRand(m_matrixRng) % 5) * m_cellHeight;

                float r1 = (float)(MatrixRand(m_matrixRng) % 1000) / 1000.0f;
                float r2 = (float)(MatrixRand(m_matrixRng) % 1000) / 1000.0f;
                switch (layer) {
                    case 0:
                        s.speed = 0.5f + r1 * 1.0f;
                        s.length = 15 + (int)(r2 * 26);
                        s.columnBrightness = 0.3f + (float)(MatrixRand(m_matrixRng) % 1000) / 5000.0f;
                        break;
                    case 1:
                        s.speed = 1.5f + r1 * 1.5f;
                        s.length = 8 + (int)(r2 * 18);
                        s.columnBrightness = 0.5f + (float)(MatrixRand(m_matrixRng) % 1000) / 3333.0f;
                        break;
                    default:
                        s.speed = 3.0f + r1 * 3.0f;
                        s.length = 5 + (int)(r2 * 11);
                        s.columnBrightness = 0.8f + (float)(MatrixRand(m_matrixRng) % 1000) / 5000.0f;
                        break;
                }
                s.chars.resize(s.length);
                for (int j = 0; j < s.length; ++j)
                    s.chars[j] = RandomMatrixChar();

                stream = std::move(s);
            }
            continue;
        }

        stream.y += stream.speed;

        // Character mutation
        float mutRate = (stream.layer == 0) ? 0.03f : (stream.layer == 1) ? 0.05f : 0.08f;
        float headRate = (stream.layer == 0) ? 0.15f : (stream.layer == 1) ? 0.20f : 0.30f;
        int charCount = static_cast<int>(stream.chars.size());
        for (int j = 0; j < charCount; ++j) {
            float rate = (j <= 2) ? headRate : mutRate;
            if ((MatrixRand(m_matrixRng) % 1000) < static_cast<uint32_t>(rate * 1000))
                stream.chars[j] = RandomMatrixChar();
        }

        // Deposit residue where the tail just left
        float tailY = stream.y - stream.length * m_cellHeight;
        int depositRow = static_cast<int>(tailY / m_cellHeight) - 1;
        if (depositRow >= 0 && depositRow < m_matrixNumRows && stream.column < m_matrixNumCols) {
            auto& rc = m_residueGrid[stream.column][depositRow];
            rc.ch = stream.chars[(std::min)(stream.length - 1, charCount - 1)];
            rc.brightness = 0.5f * stream.columnBrightness;
        }

        // Stream fully off screen
        if (tailY > static_cast<float>(m_height)) {
            stream.active = false;
            switch (stream.layer) {
                case 0: stream.cooldownRemaining = 60.0f + (float)(MatrixRand(m_matrixRng) % 121); break;
                case 1: stream.cooldownRemaining = 30.0f + (float)(MatrixRand(m_matrixRng) % 61);  break;
                default: stream.cooldownRemaining = 15.0f + (float)(MatrixRand(m_matrixRng) % 31); break;
            }
        }
    }

    // Spawn new streams in unoccupied columns
    std::vector<bool> occupied(m_matrixNumCols, false);
    for (const auto& s : m_matrixStreams)
        if (s.active && s.column >= 0 && s.column < m_matrixNumCols)
            occupied[s.column] = true;

    for (int col = 0; col < m_matrixNumCols; ++col) {
        if (occupied[col]) continue;
        uint32_t r = MatrixRand(m_matrixRng) % 10000;
        if (r < 30) SpawnStream(col, 0);
        else if (r < 90) SpawnStream(col, 1);
        else if (r < 130) SpawnStream(col, 2);
    }

    // Decay residue
    for (int col = 0; col < m_matrixNumCols; ++col) {
        for (int row = 0; row < m_matrixNumRows; ++row) {
            auto& rc = m_residueGrid[col][row];
            if (rc.brightness > 0.0f) {
                rc.brightness *= 0.985f;
                if (rc.brightness < 0.01f) {
                    rc.brightness = 0.0f;
                    rc.ch = 0;
                }
            }
        }
    }
}


void DxRenderer::InitPalette() {
    // Standard 16 colors (Windows Terminal inspired)
    m_palette[0]  = D2D1::ColorF(0x0C0C0C); // Black
    m_palette[1]  = D2D1::ColorF(0xC50F1F); // Red
    m_palette[2]  = D2D1::ColorF(0x13A10E); // Green
    m_palette[3]  = D2D1::ColorF(0xC19C00); // Yellow
    m_palette[4]  = D2D1::ColorF(0x0037DA); // Blue
    m_palette[5]  = D2D1::ColorF(0x881798); // Magenta
    m_palette[6]  = D2D1::ColorF(0x3A96DD); // Cyan
    m_palette[7]  = D2D1::ColorF(0xCCCCCC); // White
    m_palette[8]  = D2D1::ColorF(0x767676); // Bright Black
    m_palette[9]  = D2D1::ColorF(0xE74856); // Bright Red
    m_palette[10] = D2D1::ColorF(0x16C60C); // Bright Green
    m_palette[11] = D2D1::ColorF(0xF9F1A5); // Bright Yellow
    m_palette[12] = D2D1::ColorF(0x3B78FF); // Bright Blue
    m_palette[13] = D2D1::ColorF(0xB4009E); // Bright Magenta
    m_palette[14] = D2D1::ColorF(0x61D6D6); // Bright Cyan
    m_palette[15] = D2D1::ColorF(0xF2F2F2); // Bright White

    // 216 color cube (indices 16-231)
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                int idx = 16 + 36 * r + 6 * g + b;
                float fr = r ? (r * 40 + 55) / 255.0f : 0.0f;
                float fg = g ? (g * 40 + 55) / 255.0f : 0.0f;
                float fb = b ? (b * 40 + 55) / 255.0f : 0.0f;
                m_palette[idx] = D2D1::ColorF(fr, fg, fb);
            }
        }
    }

    // 24 grayscale (indices 232-255)
    for (int i = 0; i < 24; i++) {
        float v = (i * 10 + 8) / 255.0f;
        m_palette[232 + i] = D2D1::ColorF(v, v, v);
    }
}
