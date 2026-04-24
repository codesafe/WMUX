#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <memory>
#include <map>
#include <string>
#include "renderer/dx_renderer.h"
#include "pane/pane_factory.h"
#include "pane/pane_tree.h"
#include "pane/pane_session.h"
#include "settings.h"

class DropTarget;

enum class InputMode { Normal, Prefix };
enum class SelectionGranularity { Character, Word, Line };

class App {
public:
    bool Initialize(HINSTANCE hInstance, int nCmdShow,
                    const std::wstring& startupSessionId = L"",
                    const std::wstring& startupWorkingDir = L"");
    int Run();
    ~App();
    static ULONG_PTR GetAttachPaneCopyDataId();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnDropFolder(const std::wstring& path);

    void OnPaint();
    void OnResize(UINT width, UINT height);
    bool OnKeyDown(WPARAM vk, LPARAM flags);
    void OnChar(wchar_t ch);
    void OnMouseWheel(WPARAM wParam, LPARAM lParam);
    void OnLButtonDown(int x, int y);
    void OnLButtonDblClk(int x, int y);
    void OnMouseMove(int x, int y);
    void OnLButtonUp();
    void OnRButtonUp(int x, int y);
    void OnPtyOutput(WPARAM wParam, LPARAM lParam);
    void OnDpiChanged(UINT dpi, const RECT& suggestedRect);
    bool AttachExternalSession(const std::wstring& sessionId, int zone);
    PaneDescriptor DescribeActivePane() const;

    void SplitActivePane(SplitDirection dir);
    void OpenDetachablePaneWindow();
    void CloseActivePane();
    void DetachActivePaneToNewWindow();
    void RelayoutAfterPaneRemoval();
    void OpenSettings();
    void UpdateTitleBar();
    void RelayoutPanes();
    void CopySelection();
    void PasteClipboard();
    void ClearSelection();
    void SelectAllVisible(IPaneSession* pane);
    void SelectLineAt(IPaneSession* pane, int row);
    void EnterPrefixMode();
    void ExitPrefixMode();
    void UpdateSelectionAutoScroll();
    void ContinueSelectionAutoScroll();
    void ApplyVisualSettings(const Settings& settings);
    void RegisterUserActivity();
    void UpdateIdleScrambleState();
    void UpdateDragSelection(int x, int y);
    void ExpandWordSelection(IPaneSession* pane, int viewRow, int col,
                             int& startRow, int& startCol,
                             int& endRow, int& endCol);
    void ExpandLineSelection(IPaneSession* pane, int viewRow,
                             int& startRow, int& startCol,
                             int& endRow, int& endCol);
    void CancelMouseOperation();

    // Scrollbar drag helpers
    bool HitTestScrollbar(float px, float py, IPaneSession*& outPane, D2D1_RECT_F& outRect);
    void ApplyScrollbarDrag(int mouseY);
    void MouseToCell(int mx, int my, IPaneSession* pane, D2D1_RECT_F rect, int& row, int& col);

    // Separator drag helpers
    bool HitTestSeparator(float px, float py, SplitNode*& outNode);
    void ApplySeparatorDrag(int mouseX, int mouseY);

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;

    DxRenderer m_renderer;
    PaneManager m_paneManager;
    InputMode m_inputMode = InputMode::Normal;
    Settings m_settings;
    // Skip next WM_CHAR if key was already handled in WM_KEYDOWN
    // Auto-reset by TIMER_RESET_SKIP_FLAG if WM_CHAR is not delivered
    bool m_skipNextChar = false;
    bool m_oleInitialized = false;

    DropTarget* m_dropTarget = nullptr;

    // Scrollbar drag state
    bool m_draggingScrollbar = false;
    IPaneSession* m_dragPane = nullptr;
    D2D1_RECT_F m_dragPaneRect = {};
    int m_wheelDeltaRemainder = 0;

    // Separator drag state
    bool m_draggingSeparator = false;
    SplitNode* m_dragSplitNode = nullptr;
    int m_dragStartX = 0, m_dragStartY = 0;

    // Text selection state
    bool m_selecting = false;
    bool m_hasSelection = false;
    IPaneSession* m_selectPane = nullptr;
    D2D1_RECT_F m_selectPaneRect = {};
    int m_selStartRow = 0, m_selStartCol = 0;
    int m_selEndRow = 0, m_selEndCol = 0;
    int m_selAnchorStartRow = 0, m_selAnchorStartCol = 0;
    int m_selAnchorEndRow = 0, m_selAnchorEndCol = 0;
    SelectionGranularity m_selectionGranularity = SelectionGranularity::Character;
    int m_lastMouseX = 0, m_lastMouseY = 0;
    DWORD m_lastDblClickTick = 0;
    int m_lastDblClickRow = -1;
    IPaneSession* m_lastDblClickPane = nullptr;
    ULONGLONG m_lastUserInputTick = 0;
    bool m_idleScrambleActive = false;
    uint32_t m_idleScrambleFrame = 0;


    // Help popup state
    bool m_showHelp = false;
    int m_helpScrollOffset = 0;
    bool m_draggingHelpScrollbar = false;
    float m_helpDragStartY = 0;
    int m_helpScrollOffsetAtDragStart = 0;

    // IME composition state
    bool m_imeComposing = false;
    std::wstring m_imeCompStr;

    // Pane drag state
    bool m_draggingPane = false;
    IPaneSession* m_draggedPane = nullptr;
    SplitNode* m_draggedNode = nullptr;
    IPaneSession* m_dropTargetPane = nullptr;
    int m_dropZone = -1;  // 0=top, 1=right, 2=bottom, 3=left, 4=center(swap)
    HWND m_externalDropTarget = nullptr;  // target wmux window for cross-window drag

    // External drag preview (target side - when another wmux drags toward us)
    bool m_externalDragPreview = false;
    D2D1_RECT_F m_externalPreviewRect = {};
    int m_externalPreviewZone = -1;

    static UINT GetDragPreviewMsg();
    void SendDragPreview(HWND target, int screenX, int screenY);
    void CancelDragPreview(HWND target);

    static constexpr UINT WM_PTY_OUTPUT = WM_APP + 1;
    static constexpr UINT TIMER_CLOCK = 1;
    static constexpr UINT TIMER_PREFIX = 2;
    static constexpr UINT TIMER_SELECTION_AUTOSCROLL = 3;
    static constexpr UINT TIMER_IDLE_SCRAMBLE = 4;
    static constexpr UINT TIMER_FLUSH_INPUT = 5;
    static constexpr UINT TIMER_RESET_SKIP_FLAG = 6;
};
