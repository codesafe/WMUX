#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <string>
#include "terminal/buffer.h"

using Microsoft::WRL::ComPtr;

class DxRenderer {
public:
    DxRenderer() = default;
    ~DxRenderer() = default;

    bool Initialize(HWND hwnd, const std::wstring& fontName = L"Consolas",
                    float fontSize = 14.0f, uint32_t bgColor = 0x1E1E1E);
    bool UpdateFont(const std::wstring& fontName, float fontSize);
    void SetBackgroundColor(uint32_t rgb);
    void Resize(UINT width, UINT height);
    void Render(const TerminalBuffer& buffer);

    // Multi-pane rendering
    bool BeginFrame();
    struct Selection { int startRow, startCol, endRow, endCol; bool active; };

    void RenderPane(const TerminalBuffer& buffer, D2D1_RECT_F rect,
                    bool isActive, bool isZoomed, bool scrollbarDragging = false,
                    const Selection* sel = nullptr, bool dimInactive = true);
    void RenderSeparator(float x1, float y1, float x2, float y2);
    void RenderStatusBar(float y, float width, const std::wstring& text);
    void RenderPrefixIndicator();
    void EndFrame();

    float GetStatusBarHeight() const { return m_cellHeight + 4.0f; }

    int CalcCols(UINT width) const;
    int CalcRows(UINT height) const;
    float GetCellWidth() const { return m_cellWidth; }
    float GetCellHeight() const { return m_cellHeight; }
    static constexpr float GetPanePadding() { return 4.0f; }

private:
    bool CreateDeviceResources();
    void DiscardDeviceResources();
    bool MeasureCellSize();
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
