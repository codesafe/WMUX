#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <memory>
#include "renderer/dx_renderer.h"
#include "pane/pane_tree.h"
#include "settings.h"

enum class InputMode { Normal, Prefix };

class App {
public:
    bool Initialize(HINSTANCE hInstance, int nCmdShow);
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnPaint();
    void OnResize(UINT width, UINT height);
    void OnKeyDown(WPARAM vk, LPARAM flags);
    void OnChar(wchar_t ch);
    void OnMouseWheel(WPARAM wParam, LPARAM lParam);
    void OnLButtonDown(int x, int y);
    void OnLButtonDblClk(int x, int y);
    void OnMouseMove(int x, int y);
    void OnLButtonUp();
    void OnRButtonUp(int x, int y);
    void OnPtyOutput(WPARAM wParam, LPARAM lParam);

    void SplitActivePane(SplitDirection dir);
    void CloseActivePane();
    void OpenSettings();
    void UpdateTitleBar();
    void CopySelection();
    void PasteClipboard();
    void ClearSelection();

    // Scrollbar drag helpers
    bool HitTestScrollbar(float px, float py, Pane*& outPane, D2D1_RECT_F& outRect);
    void ApplyScrollbarDrag(int mouseY);
    void MouseToCell(int mx, int my, D2D1_RECT_F rect, int& row, int& col);

    // Separator drag helpers
    bool HitTestSeparator(float px, float py, SplitNode*& outNode);
    void ApplySeparatorDrag(int mouseX, int mouseY);

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;

    DxRenderer m_renderer;
    PaneManager m_paneManager;
    InputMode m_inputMode = InputMode::Normal;
    Settings m_settings;
    bool m_skipNextChar = false;  // Flag to skip WM_CHAR after handling in WM_KEYDOWN

    // Scrollbar drag state
    bool m_draggingScrollbar = false;
    Pane* m_dragPane = nullptr;
    D2D1_RECT_F m_dragPaneRect = {};

    // Separator drag state
    bool m_draggingSeparator = false;
    SplitNode* m_dragSplitNode = nullptr;
    int m_dragStartX = 0, m_dragStartY = 0;

    // Text selection state
    bool m_selecting = false;
    bool m_hasSelection = false;
    Pane* m_selectPane = nullptr;
    D2D1_RECT_F m_selectPaneRect = {};
    int m_selStartRow = 0, m_selStartCol = 0;
    int m_selEndRow = 0, m_selEndCol = 0;

    static constexpr UINT WM_PTY_OUTPUT = WM_APP + 1;
};
