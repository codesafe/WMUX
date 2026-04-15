#include "app.h"
#include "settings_dialog.h"

bool App::Initialize(HINSTANCE hInstance, int nCmdShow) {
    m_hInstance = hInstance;
    m_settings.Load();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"WmuxWindowClass";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
        return false;

    m_hwnd = CreateWindowExW(
        0, L"WmuxWindowClass", L"wmux",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        m_settings.windowWidth, m_settings.windowHeight,
        nullptr, nullptr, hInstance, this);

    if (!m_hwnd)
        return false;

    if (!m_renderer.Initialize(m_hwnd, m_settings.fontName, m_settings.fontSize))
        return false;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    float statusH = m_renderer.GetStatusBarHeight();
    D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right),
                             static_cast<float>(rc.bottom) - statusH};

    if (!m_paneManager.Initialize(paneRect, m_hwnd, WM_PTY_OUTPUT,
                                   m_renderer.GetCellWidth(),
                                   m_renderer.GetCellHeight()))
        return false;

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    // Timer for status bar clock update (1 second)
    SetTimer(m_hwnd, 1, 1000, nullptr);

    return true;
}

int App::Run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;

    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        app = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (app)
        return app->HandleMessage(msg, wParam, lParam);

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(m_hwnd, &ps);
        OnPaint();
        EndPaint(m_hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN:
        OnKeyDown(wParam, lParam);
        return 0;

    case WM_CHAR:
        OnChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_MOUSEWHEEL:
        OnMouseWheel(wParam, lParam);
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                      static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;

    case WM_LBUTTONDBLCLK:
        OnLButtonDblClk(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                        static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;

    case WM_MOUSEMOVE: {
        int mx = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        int my = static_cast<int>(static_cast<short>(HIWORD(lParam)));

        if (m_draggingSeparator || m_draggingScrollbar || m_selecting) {
            OnMouseMove(mx, my);
        } else {
            // Update cursor when hovering over separator
            SplitNode* node = nullptr;
            if (HitTestSeparator(static_cast<float>(mx), static_cast<float>(my), node)) {
                if (node->direction == SplitDirection::Vertical)
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                else
                    SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            } else {
                SetCursor(LoadCursor(nullptr, IDC_IBEAM));
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (m_draggingScrollbar || m_selecting)
            OnLButtonUp();
        return 0;

    case WM_RBUTTONUP:
        OnRButtonUp(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;

    case WM_TIMER:
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;

    case WM_PTY_OUTPUT:
        OnPtyOutput(wParam, lParam);
        return 0;

    case WM_CLOSE: {
        // Save window size
        RECT wr;
        GetWindowRect(m_hwnd, &wr);
        m_settings.windowWidth = wr.right - wr.left;
        m_settings.windowHeight = wr.bottom - wr.top;
        m_settings.Save();

        m_paneManager.ForEachLeaf([](SplitNode& node) {
            node.pane->Stop();
        });
        DestroyWindow(m_hwnd);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(m_hwnd, msg, wParam, lParam);
    }
}

void App::OnPaint() {
    if (!m_renderer.BeginFrame()) return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    float clientW = static_cast<float>(rc.right);
    float clientH = static_cast<float>(rc.bottom);
    float statusH = m_renderer.GetStatusBarHeight();
    float paneAreaH = clientH - statusH;

    if (m_paneManager.IsZoomed()) {
        SplitNode* node = m_paneManager.GetActiveNode();
        if (node && node->pane) {
            D2D1_RECT_F fullRect = {0, 0, clientW, paneAreaH};
            int cols = m_renderer.CalcCols(rc.right);
            int rows = m_renderer.CalcRows(static_cast<UINT>(paneAreaH));
            node->pane->Resize(cols, rows);
            bool dragging = m_draggingScrollbar && m_dragPane == node->pane.get();
            DxRenderer::Selection* pSel = nullptr;
            DxRenderer::Selection sel{};
            if ((m_hasSelection || m_selecting) && m_selectPane == node->pane.get()) {
                sel = {m_selStartRow, m_selStartCol, m_selEndRow, m_selEndCol, true};
                pSel = &sel;
            }
            m_renderer.RenderPane(node->pane->GetBuffer(), fullRect, true, true, dragging, pSel);
        }
    } else {
        SplitNode* activeNode = m_paneManager.GetActiveNode();
        m_paneManager.ForEachLeaf([&](SplitNode& node) {
            bool isActive = (&node == activeNode);
            bool dragging = m_draggingScrollbar && m_dragPane == node.pane.get();
            DxRenderer::Selection* pSel = nullptr;
            DxRenderer::Selection sel{};
            if ((m_hasSelection || m_selecting) && m_selectPane == node.pane.get()) {
                sel = {m_selStartRow, m_selStartCol, m_selEndRow, m_selEndCol, true};
                pSel = &sel;
            }
            m_renderer.RenderPane(node.pane->GetBuffer(), node.rect, isActive, false, dragging, pSel);
        });

        std::vector<PaneManager::SeparatorLine> seps;
        m_paneManager.CollectSeparators(seps);
        for (auto& s : seps)
            m_renderer.RenderSeparator(s.x1, s.y1, s.x2, s.y2);
    }

    // Status bar
    std::wstring statusText;
    int paneCount = 0;
    int currentPaneIndex = 0;
    int index = 0;
    Pane* active = m_paneManager.GetActivePane();

    m_paneManager.ForEachLeaf([&](SplitNode& node) {
        index++;
        if (node.pane.get() == active) {
            currentPaneIndex = index;
        }
        paneCount++;
    });

    if (active) {
        std::string title = active->GetBuffer().GetTitle();
        if (title.empty()) title = "cmd";

        int titleLen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
        std::wstring wtitle(titleLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle.data(), titleLen);
        if (!wtitle.empty() && wtitle.back() == L'\0') wtitle.pop_back();

        // Time
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t timeBuf[16];
        wsprintfW(timeBuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

        statusText = L" [" + std::to_wstring(currentPaneIndex) + L"/" + std::to_wstring(paneCount) + L"] " + wtitle;
        if (m_paneManager.IsZoomed()) statusText += L" [ZOOM]";
        if (m_inputMode == InputMode::Prefix) statusText += L" [PREFIX]";

        // Right-align time
        size_t pad = 0;
        int totalCols = m_renderer.CalcCols(rc.right);
        int usedCols = static_cast<int>(statusText.size()) + 10;
        if (totalCols > usedCols) pad = totalCols - usedCols;
        statusText += std::wstring(pad, L' ') + timeBuf + L" ";
    }

    m_renderer.RenderStatusBar(paneAreaH, clientW, statusText);

    if (m_inputMode == InputMode::Prefix)
        m_renderer.RenderPrefixIndicator();

    m_renderer.EndFrame();
}

void App::OnResize(UINT width, UINT height) {
    m_renderer.Resize(width, height);
    float statusH = m_renderer.GetStatusBarHeight();
    float paneH = static_cast<float>(height) - statusH;
    if (paneH < 1.0f) paneH = 1.0f;
    D2D1_RECT_F paneRect = {0, 0, static_cast<float>(width), paneH};
    m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(),
                           m_renderer.GetCellHeight());
}

void App::OnKeyDown(WPARAM vk, LPARAM /*flags*/) {
    // Prefix mode: handle all commands here (bypasses IME for Korean input)
    if (m_inputMode == InputMode::Prefix) {
        bool handled = true;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        // Arrow keys for pane navigation
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) {
            switch (vk) {
            case VK_UP:    m_paneManager.MoveFocus(SplitDirection::Horizontal, false); break;
            case VK_DOWN:  m_paneManager.MoveFocus(SplitDirection::Horizontal, true);  break;
            case VK_LEFT:  m_paneManager.MoveFocus(SplitDirection::Vertical, false);   break;
            case VK_RIGHT: m_paneManager.MoveFocus(SplitDirection::Vertical, true);    break;
            }
        }
        // Character commands (VK code check bypasses IME)
        else if (vk == 'V' || vk == '5') {  // v or % (both work)
            SplitActivePane(SplitDirection::Vertical);
        }
        else if (vk == 'H' || vk == '2') {  // h or " (both work)
            SplitActivePane(SplitDirection::Horizontal);
        }
        else if (vk == 'X') {
            CloseActivePane();
        }
        else if (vk == 'Z') {
            m_paneManager.ToggleZoom();
            if (!m_paneManager.IsZoomed()) {
                RECT rc;
                GetClientRect(m_hwnd, &rc);
                float sH = m_renderer.GetStatusBarHeight();
                D2D1_RECT_F clientRect = {0, 0, static_cast<float>(rc.right),
                                           static_cast<float>(rc.bottom) - sH};
                m_paneManager.Relayout(clientRect, m_renderer.GetCellWidth(),
                                       m_renderer.GetCellHeight());
            }
        }
        else if (vk == 'O') {
            OpenSettings();
        }
        else if (vk == 'B' && ctrl) {  // Ctrl+B: send literal 0x02
            if (auto* p = m_paneManager.GetActivePane())
                p->SendInput("\x02", 1);
        }
        else {
            handled = false;
        }

        if (handled) {
            m_inputMode = InputMode::Normal;
            UpdateTitleBar();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    // Normal mode
    Pane* active = m_paneManager.GetActivePane();
    if (!active) return;

    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // Ctrl+B: enter prefix mode (must be in OnKeyDown to bypass IME)
    if (ctrl && vk == 'B') {
        m_inputMode = InputMode::Prefix;
        UpdateTitleBar();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Ctrl+C: copy if selection exists, otherwise send SIGINT (0x03) via WM_CHAR
    if (ctrl && vk == 'C' && m_hasSelection) {
        CopySelection();
        return;
    }
    // Ctrl+A: select all visible content in active pane
    if (ctrl && vk == 'A') {
        auto& buf = active->GetBuffer();
        m_selectPane = active;
        m_paneManager.ForEachLeaf([&](SplitNode& node) {
            if (node.pane.get() == active)
                m_selectPaneRect = node.rect;
        });
        m_selStartRow = 0;
        m_selStartCol = 0;
        m_selEndRow = buf.GetRows() - 1;
        m_selEndCol = buf.GetCols() - 1;
        m_hasSelection = true;
        m_selecting = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    // Ctrl+V: paste
    if (ctrl && vk == 'V') {
        PasteClipboard();
        return;
    }

    // Shift+Arrow: keyboard text selection (Home/End pass through to shell)
    if (shift && !ctrl && (vk == VK_LEFT || vk == VK_RIGHT ||
                           vk == VK_UP || vk == VK_DOWN)) {
        auto& buf = active->GetBuffer();
        Pane* pane = active;
        D2D1_RECT_F paneRect = {};
        // Find pane rect
        m_paneManager.ForEachLeaf([&](SplitNode& node) {
            if (node.pane.get() == pane)
                paneRect = node.rect;
        });

        if (!m_hasSelection && !m_selecting) {
            // Start selection at cursor position
            m_selectPane = pane;
            m_selectPaneRect = paneRect;
            m_selStartRow = buf.GetCursorRow();
            m_selStartCol = buf.GetCursorCol();
            m_selEndRow = m_selStartRow;
            m_selEndCol = m_selStartCol;
            m_hasSelection = true;
        }

        // Extend selection end
        switch (vk) {
        case VK_LEFT:
            if (m_selEndCol > 0) m_selEndCol--;
            else if (m_selEndRow > 0) { m_selEndRow--; m_selEndCol = buf.GetCols() - 1; }
            break;
        case VK_RIGHT:
            if (m_selEndCol < buf.GetCols() - 1) m_selEndCol++;
            else if (m_selEndRow < buf.GetRows() - 1) { m_selEndRow++; m_selEndCol = 0; }
            break;
        case VK_UP:
            if (m_selEndRow > 0) m_selEndRow--;
            break;
        case VK_DOWN:
            if (m_selEndRow < buf.GetRows() - 1) m_selEndRow++;
            break;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Any non-shift arrow key clears keyboard selection
    if (!shift && m_hasSelection && !m_selecting &&
        (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
        ClearSelection();
    }

    // Shift+PageUp/Down: scrollback navigation
    if (shift && (vk == VK_PRIOR || vk == VK_NEXT)) {
        int pageSize = active->GetBuffer().GetRows() / 2;
        if (pageSize < 1) pageSize = 1;
        if (vk == VK_PRIOR)
            active->GetBuffer().ScrollBack(pageSize);
        else
            active->GetBuffer().ScrollForward(pageSize);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Ctrl+Arrow: pane focus navigation
    if (ctrl && (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)) {
        switch (vk) {
        case VK_UP:    m_paneManager.MoveFocus(SplitDirection::Horizontal, false); break;
        case VK_DOWN:  m_paneManager.MoveFocus(SplitDirection::Horizontal, true);  break;
        case VK_LEFT:  m_paneManager.MoveFocus(SplitDirection::Vertical, false);   break;
        case VK_RIGHT: m_paneManager.MoveFocus(SplitDirection::Vertical, true);    break;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Send VT sequences to active pane
    std::string seq;
    bool appCur = active->GetBuffer().IsAppCursorKeys();

    switch (vk) {
    case VK_UP:     seq = appCur ? "\x1bOA" : "\x1b[A"; break;
    case VK_DOWN:   seq = appCur ? "\x1bOB" : "\x1b[B"; break;
    case VK_RIGHT:  seq = appCur ? "\x1bOC" : "\x1b[C"; break;
    case VK_LEFT:   seq = appCur ? "\x1bOD" : "\x1b[D"; break;
    case VK_HOME:   seq = "\x1b[H"; break;
    case VK_END:    seq = "\x1b[F"; break;
    case VK_INSERT: seq = "\x1b[2~"; break;
    case VK_DELETE: seq = "\x1b[3~"; break;
    case VK_PRIOR:  seq = "\x1b[5~"; break;
    case VK_NEXT:   seq = "\x1b[6~"; break;
    case VK_F1:     seq = "\x1bOP"; break;
    case VK_F2:     seq = "\x1bOQ"; break;
    case VK_F3:     seq = "\x1bOR"; break;
    case VK_F4:     seq = "\x1bOS"; break;
    case VK_F5:     seq = "\x1b[15~"; break;
    case VK_F6:     seq = "\x1b[17~"; break;
    case VK_F7:     seq = "\x1b[18~"; break;
    case VK_F8:     seq = "\x1b[19~"; break;
    case VK_F9:     seq = "\x1b[20~"; break;
    case VK_F10:    seq = "\x1b[21~"; break;
    case VK_F11:    seq = "\x1b[23~"; break;
    case VK_F12:    seq = "\x1b[24~"; break;
    default: return;
    }

    active->SendInput(seq);
}

void App::OnChar(wchar_t ch) {
    // Prefix key detection (now handled in OnKeyDown for IME compatibility)
    if (m_inputMode == InputMode::Normal) {
        if (ch == 0x02) { // Ctrl+B (already handled in OnKeyDown, skip)
            return;
        }
        // Ctrl+V (0x16) handled in OnKeyDown
        if (ch == 0x16) return;
        // Backspace: send DEL (0x7F) instead of BS (0x08)
        if (ch == 0x08) {
            Pane* active = m_paneManager.GetActivePane();
            if (active) {
                active->GetBuffer().ScrollToBottom();
                active->SendInput("\x7f", 1);
            }
            return;
        }

        Pane* active = m_paneManager.GetActivePane();
        if (!active) return;

        // Auto-scroll to bottom on typing
        active->GetBuffer().ScrollToBottom();

        char utf8[4];
        int len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, sizeof(utf8),
                                      nullptr, nullptr);
        if (len > 0)
            active->SendInput(utf8, len);
        return;
    }

    // Prefix mode commands
    m_inputMode = InputMode::Normal;
    UpdateTitleBar();

    switch (ch) {
    case 0x02: // Double Ctrl+B: send literal to pane
        if (auto* p = m_paneManager.GetActivePane())
            p->SendInput("\x02", 1);
        break;
    case '%': case 'v':
        SplitActivePane(SplitDirection::Vertical);
        break;
    case '"': case 'h':
        SplitActivePane(SplitDirection::Horizontal);
        break;
    case 'x':
        CloseActivePane();
        break;
    case 'z':
        m_paneManager.ToggleZoom();
        if (!m_paneManager.IsZoomed()) {
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            float sH = m_renderer.GetStatusBarHeight();
            D2D1_RECT_F clientRect = {0, 0, static_cast<float>(rc.right),
                                       static_cast<float>(rc.bottom) - sH};
            m_paneManager.Relayout(clientRect, m_renderer.GetCellWidth(),
                                   m_renderer.GetCellHeight());
        }
        break;
    case 'o':
        OpenSettings();
        break;
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::OnPtyOutput(WPARAM wParam, LPARAM lParam) {
    uint32_t paneId = static_cast<uint32_t>(lParam);
    Pane* pane = m_paneManager.FindPaneById(paneId);
    if (!pane) return;

    if (wParam == 1) {
        // Process exited - close this pane
        if (!m_paneManager.ClosePaneById(paneId)) {
            // Last pane closed
            DestroyWindow(m_hwnd);
            return;
        }
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        float statusH = m_renderer.GetStatusBarHeight();
        D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right),
                                 static_cast<float>(rc.bottom) - statusH};
        m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(),
                               m_renderer.GetCellHeight());
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    pane->ProcessOutput();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::OnMouseWheel(WPARAM wParam, LPARAM lParam) {
    short delta = static_cast<short>(HIWORD(wParam));

    // Screen coords → client coords
    POINT pt;
    pt.x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
    pt.y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
    ScreenToClient(m_hwnd, &pt);

    Pane* pane = m_paneManager.FindPaneAtPoint(
        static_cast<float>(pt.x), static_cast<float>(pt.y));
    if (!pane)
        pane = m_paneManager.GetActivePane();
    if (!pane) return;

    int lines = 3;
    if (delta > 0)
        pane->GetBuffer().ScrollBack(lines);
    else
        pane->GetBuffer().ScrollForward(lines);

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::SplitActivePane(SplitDirection dir) {
    m_paneManager.SplitActive(dir, m_hwnd, WM_PTY_OUTPUT,
                               m_renderer.GetCellWidth(),
                               m_renderer.GetCellHeight());
}

void App::CloseActivePane() {
    if (!m_paneManager.CloseActive()) {
        // Last pane closed
        DestroyWindow(m_hwnd);
        return;
    }

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_RECT_F clientRect = {0, 0, static_cast<float>(rc.right),
                               static_cast<float>(rc.bottom)};
    m_paneManager.Relayout(clientRect, m_renderer.GetCellWidth(),
                           m_renderer.GetCellHeight());
}

bool App::HitTestScrollbar(float px, float py, Pane*& outPane, D2D1_RECT_F& outRect) {
    if (!m_paneManager.FindPaneAndRectAtPoint(px, py, outPane, outRect))
        return false;

    int sbSize = outPane->GetBuffer().GetScrollbackSize();
    if (sbSize <= 0) return false;

    float barW = 8.0f;
    float barX = outRect.right - barW;
    return px >= barX;
}

bool App::HitTestSeparator(float px, float py, SplitNode*& outNode) {
    outNode = m_paneManager.FindSeparatorAtPoint(px, py);
    return outNode != nullptr;
}

void App::ApplySeparatorDrag(int mouseX, int mouseY) {
    if (!m_dragSplitNode) return;
    m_paneManager.UpdateSplitRatio(m_dragSplitNode,
                                     static_cast<float>(mouseX),
                                     static_cast<float>(mouseY),
                                     m_renderer.GetCellWidth(),
                                     m_renderer.GetCellHeight());
}

void App::ApplyScrollbarDrag(int mouseY) {
    if (!m_dragPane) return;

    auto& buf = m_dragPane->GetBuffer();
    int sbSize = buf.GetScrollbackSize();
    if (sbSize <= 0) return;

    int rows = buf.GetRows();
    int totalLines = sbSize + rows;
    float paneH = m_dragPaneRect.bottom - m_dragPaneRect.top;
    float thumbRatio = static_cast<float>(rows) / totalLines;
    float thumbH = (std::max)(20.0f, paneH * thumbRatio);
    float scrollRange = paneH - thumbH;
    if (scrollRange <= 0) return;

    float relY = static_cast<float>(mouseY) - m_dragPaneRect.top;
    float pos = relY - thumbH * 0.5f;
    pos = (std::max)(0.0f, (std::min)(pos, scrollRange));

    float ratio = 1.0f - pos / scrollRange;
    int offset = static_cast<int>(ratio * sbSize + 0.5f);
    offset = (std::max)(0, (std::min)(offset, sbSize));

    buf.ScrollToBottom();
    buf.ScrollBack(offset);
}

void App::MouseToCell(int mx, int my, D2D1_RECT_F rect, int& row, int& col) {
    float cw = m_renderer.GetCellWidth();
    float ch = m_renderer.GetCellHeight();
    col = static_cast<int>((mx - rect.left) / cw);
    row = static_cast<int>((my - rect.top) / ch);
    if (col < 0) col = 0;
    if (row < 0) row = 0;
}

static bool IsWordChar(wchar_t ch) {
    if (ch <= L' ') return false;
    // Treat common delimiters as non-word
    return ch != L' ' && ch != L'\t' && ch != L'(' && ch != L')' &&
           ch != L'[' && ch != L']' && ch != L'{' && ch != L'}' &&
           ch != L'<' && ch != L'>' && ch != L'"' && ch != L'\'' &&
           ch != L',' && ch != L';' && ch != L'`';
}

void App::OnLButtonDblClk(int x, int y) {
    ClearSelection();
    Pane* pane = nullptr;
    D2D1_RECT_F rect = {};
    if (!m_paneManager.FindPaneAndRectAtPoint(
            static_cast<float>(x), static_cast<float>(y), pane, rect))
        return;

    int row, col;
    MouseToCell(x, y, rect, row, col);

    auto& buf = pane->GetBuffer();
    int cols = buf.GetCols();
    if (row >= buf.GetRows() || col >= cols) return;

    const Cell& clicked = (buf.GetScrollOffset() > 0)
        ? buf.ViewAt(row, col) : buf.At(row, col);
    if (!IsWordChar(clicked.ch)) return;

    // Expand left
    int left = col;
    while (left > 0) {
        const Cell& c = (buf.GetScrollOffset() > 0)
            ? buf.ViewAt(row, left - 1) : buf.At(row, left - 1);
        if (!IsWordChar(c.ch)) break;
        left--;
    }
    // Expand right
    int right = col;
    while (right < cols - 1) {
        const Cell& c = (buf.GetScrollOffset() > 0)
            ? buf.ViewAt(row, right + 1) : buf.At(row, right + 1);
        if (!IsWordChar(c.ch)) break;
        right++;
    }

    m_selectPane = pane;
    m_selectPaneRect = rect;
    m_selStartRow = row;
    m_selStartCol = left;
    m_selEndRow = row;
    m_selEndCol = right;
    m_hasSelection = true;
    m_selecting = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::OnLButtonDown(int x, int y) {
    // Separator drag (highest priority)
    SplitNode* splitNode = nullptr;
    if (HitTestSeparator(static_cast<float>(x), static_cast<float>(y), splitNode)) {
        m_draggingSeparator = true;
        m_dragSplitNode = splitNode;
        m_dragStartX = x;
        m_dragStartY = y;
        SetCapture(m_hwnd);
        // Change cursor to resize
        if (splitNode->direction == SplitDirection::Vertical)
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        else
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
        return;
    }

    // Scrollbar drag
    Pane* pane = nullptr;
    D2D1_RECT_F rect = {};
    if (HitTestScrollbar(static_cast<float>(x), static_cast<float>(y), pane, rect)) {
        m_draggingScrollbar = true;
        m_dragPane = pane;
        m_dragPaneRect = rect;
        SetCapture(m_hwnd);
        ApplyScrollbarDrag(y);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Pane activation and text selection
    ClearSelection();
    if (m_paneManager.FindPaneAndRectAtPoint(
            static_cast<float>(x), static_cast<float>(y), pane, rect)) {
        // Activate clicked pane
        m_paneManager.SetActivePane(pane);

        // Start text selection
        m_selecting = true;
        m_selectPane = pane;
        m_selectPaneRect = rect;
        MouseToCell(x, y, rect, m_selStartRow, m_selStartCol);
        m_selEndRow = m_selStartRow;
        m_selEndCol = m_selStartCol;
        SetCapture(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void App::OnMouseMove(int x, int y) {
    if (m_draggingSeparator) {
        ApplySeparatorDrag(x, y);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_draggingScrollbar) {
        ApplyScrollbarDrag(y);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_selecting) {
        MouseToCell(x, y, m_selectPaneRect, m_selEndRow, m_selEndCol);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void App::OnLButtonUp() {
    if (m_draggingSeparator) {
        m_draggingSeparator = false;
        m_dragSplitNode = nullptr;
        ReleaseCapture();
        SetCursor(LoadCursor(nullptr, IDC_IBEAM));
    } else if (m_draggingScrollbar) {
        m_draggingScrollbar = false;
        m_dragPane = nullptr;
        ReleaseCapture();
    } else if (m_selecting) {
        m_selecting = false;
        m_hasSelection = (m_selStartRow != m_selEndRow || m_selStartCol != m_selEndCol);
        ReleaseCapture();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::OnRButtonUp(int x, int y) {
    (void)x; (void)y;
    if (m_hasSelection) {
        CopySelection();
        ClearSelection();
    } else {
        PasteClipboard();
    }
}

void App::ClearSelection() {
    m_hasSelection = false;
    m_selecting = false;
    m_selectPane = nullptr;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::CopySelection() {
    if (!m_hasSelection || !m_selectPane) return;

    int sr = m_selStartRow, sc = m_selStartCol;
    int er = m_selEndRow, ec = m_selEndCol;
    if (sr > er || (sr == er && sc > ec)) {
        std::swap(sr, er); std::swap(sc, ec);
    }

    auto& buf = m_selectPane->GetBuffer();
    int cols = buf.GetCols();
    std::wstring text;

    for (int r = sr; r <= er; r++) {
        int cStart = (r == sr) ? sc : 0;
        int cEnd = (r == er) ? ec : cols - 1;
        for (int c = cStart; c <= cEnd && c < cols; c++) {
            const Cell& cell = buf.GetScrollOffset() > 0
                ? buf.ViewAt(r, c) : buf.At(r, c);
            if (cell.width == 0) continue;
            if (cell.ch != 0)
                text += cell.ch;
            if (cell.ch2 != 0)
                text += cell.ch2;
        }
        // Trim trailing spaces on each line
        while (!text.empty() && text.back() == L' ')
            text.pop_back();
        if (r < er) text += L'\n';
    }

    if (text.empty()) return;

    if (OpenClipboard(m_hwnd)) {
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem));
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }

    ClearSelection();
}

void App::PasteClipboard() {
    Pane* active = m_paneManager.GetActivePane();
    if (!active) return;

    if (!OpenClipboard(m_hwnd)) return;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        wchar_t* src = static_cast<wchar_t*>(GlobalLock(hData));
        if (src) {
            int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                std::string utf8(len - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, src, -1, utf8.data(), len, nullptr, nullptr);

                // Bracketed paste if supported
                if (active->GetBuffer().IsBracketedPaste())
                    active->SendInput("\x1b[200~");
                active->SendInput(utf8);
                if (active->GetBuffer().IsBracketedPaste())
                    active->SendInput("\x1b[201~");
            }
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
}

void App::OpenSettings() {
    if (ShowSettingsDialog(m_hwnd, m_settings)) {
        m_renderer.UpdateFont(m_settings.fontName, m_settings.fontSize);

        RECT rc;
        GetClientRect(m_hwnd, &rc);
        float statusH = m_renderer.GetStatusBarHeight();
        D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right),
                                 static_cast<float>(rc.bottom) - statusH};
        m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(),
                               m_renderer.GetCellHeight());
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void App::UpdateTitleBar() {
    if (m_inputMode == InputMode::Prefix)
        SetWindowTextW(m_hwnd, L"wmux [PREFIX]");
    else
        SetWindowTextW(m_hwnd, L"wmux");
}
