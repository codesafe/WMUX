#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <string>
#include <map>
#include "terminal/buffer.h"

using Microsoft::WRL::ComPtr;

class DxRenderer {
public:
    DxRenderer() = default;
    ~DxRenderer() = default;

    bool Initialize(HWND hwnd, const std::wstring& fontName = L"Consolas",
                    float fontSize = 14.0f, uint32_t bgColor = 0x1E1E1E);
    bool SetDpi(UINT dpi);
    bool UpdateFont(const std::wstring& fontName, float fontSize);
    void SetBackgroundColor(uint32_t rgb);
    void Resize(UINT width, UINT height);
    void Render(const TerminalBuffer& buffer);

    // Multi-pane rendering
    bool BeginFrame();
    struct Selection {
        int startDocumentRow, startCol;
        int endDocumentRow, endCol;
        bool active;
    };
    struct IdleEffect {
        bool active = false;
        uint32_t frame = 0;
        const std::map<std::pair<int, int>, Cell>* scrambledCells = nullptr;
    };

    void RenderPane(const TerminalBuffer& buffer, D2D1_RECT_F rect,
                    bool isActive, bool isZoomed, bool scrollbarDragging = false,
                    const Selection* sel = nullptr, bool dimInactive = true,
                    const IdleEffect* idleEffect = nullptr);
    void RenderSeparator(float x1, float y1, float x2, float y2);
    void RenderStatusBar(float y, float width, const std::wstring& leftText,
                         const std::wstring& rightText = L"", bool isZoomed = false);
    void RenderZoomBorder(float width, float height);
    void RenderPrefixIndicator();
    void RenderPrefixOverlay(const std::wstring& text);
    void RenderHelpPopup(int scrollOffset);
    void RenderDropZone(D2D1_RECT_F rect, int zone);
    void EndFrame();

    float GetStatusBarHeight() const { return m_cellHeight + 4.0f; }

    int CalcCols(UINT width) const;
    int CalcRows(UINT height) const;
    float GetCellWidth() const { return m_cellWidth; }
    float GetCellHeight() const { return m_cellHeight; }
    static constexpr float GetPanePadding() { return 4.0f; }

    // Help popup constants
    static constexpr float HELP_POPUP_WIDTH = 700.0f;
    static constexpr int HELP_TOTAL_LINES = 47;
    static constexpr int HELP_VISIBLE_LINES = 20;
    static constexpr float HELP_POPUP_PADDING = 20.0f;
    static constexpr float HELP_SCROLLBAR_WIDTH = 8.0f;
    static constexpr float HELP_DRAG_SENSITIVITY = 8.0f;  // pixels per line

private:
    bool CreateDeviceResources();
    void DiscardDeviceResources();
    bool RecreateTextFormat();
    bool MeasureCellSize();
    float MeasureTextWidth(const std::wstring& text) const;
    void InitPalette();

    D2D1_COLOR_F GetCellFgColor(const Cell& cell) const;
    D2D1_COLOR_F GetCellBgColor(const Cell& cell) const;

    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    float m_cellWidth = 0;
    float m_cellHeight = 0;
    float m_fontSize = 14.0f;
    float m_baseline = 0;
    UINT m_dpi = 96;
    std::wstring m_fontName = L"Consolas";

    ComPtr<ID2D1Factory> m_pFactory;
    ComPtr<ID2D1HwndRenderTarget> m_pRenderTarget;
    ComPtr<IDWriteFactory> m_pDWriteFactory;
    ComPtr<IDWriteTextFormat> m_pTextFormat;
    ComPtr<ID2D1SolidColorBrush> m_pBrush;

    D2D1_COLOR_F m_defaultFg = {0.80f, 0.80f, 0.80f, 1.0f};
    D2D1_COLOR_F m_defaultBg = {0.12f, 0.12f, 0.12f, 1.0f};

    std::array<D2D1_COLOR_F, 256> m_palette;
};
