#include "app.h"
#include "settings_dialog.h"
#include "drop_target.h"
#include "url_detect.h"
#include <imm.h>
#include <ole2.h>
#include <shellapi.h>
#include <sstream>

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "ole32.lib")

#ifndef VK_PROCESSKEY
#define VK_PROCESSKEY 0xE5
#endif

static int CharClass(wchar_t ch);
static bool IsWordChar(wchar_t ch);
static void AppendQuotedArg(std::wstring& command, const std::wstring& arg);
static UINT NormalizeKey(WPARAM vk, LPARAM flags);

static void AppendRuntimeLog(const std::wstring& message) {
    wchar_t tempPath[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempPath);
    if (len == 0 || len >= MAX_PATH)
        return;

    std::wstring path = tempPath;
    path += L"wmux_runtime.log";

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wstringstream line;
    line << L"[app " << GetCurrentProcessId() << L"] "
         << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"."
         << st.wMilliseconds << L" " << message << L"\r\n";
    std::wstring text = line.str();

    DWORD written = 0;
    WriteFile(file, text.data(),
              static_cast<DWORD>(text.size() * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(file);
}

static int CountVisibleCells(const TerminalBuffer& buffer) {
    int count = 0;
    for (int r = 0; r < buffer.GetRows(); ++r) {
        for (int c = 0; c < buffer.GetCols(); ++c) {
            const Cell& cell = buffer.At(r, c);
            if (cell.width > 0 && cell.ch != 0 && cell.ch != L' ')
                count++;
        }
    }
    return count;
}

static UINT NormalizeKey(WPARAM vk, LPARAM flags) {
    UINT key = static_cast<UINT>(vk);
    if (key != VK_PROCESSKEY)
        return key;

    UINT scanCode = (static_cast<UINT>(flags) >> 16) & 0xFF;
    if (flags & 0x01000000)
        scanCode |= 0xE000;

    UINT mapped = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
    return mapped != 0 ? mapped : key;
}

ULONG_PTR App::GetAttachPaneCopyDataId() {
    static constexpr ULONG_PTR kAttachPaneCopyDataId = 0x574D5559; // 'WMUY'
    return kAttachPaneCopyDataId;
}

UINT App::GetDragPreviewMsg() {
    static UINT msg = RegisterWindowMessageW(L"WmuxDragPreview");
    return msg;
}

void App::SendDragPreview(HWND target, int screenX, int screenY) {
    PostMessage(target, GetDragPreviewMsg(), 0,
                MAKELPARAM(static_cast<WORD>(static_cast<short>(screenX)),
                           static_cast<WORD>(static_cast<short>(screenY))));
}

void App::CancelDragPreview(HWND target) {
    PostMessage(target, GetDragPreviewMsg(), 1, 0);
}

App::~App() {
    if (m_hwnd && m_dropTarget) {
        RevokeDragDrop(m_hwnd);
        m_dropTarget->Release();
        m_dropTarget = nullptr;
    }
    if (m_oleInitialized) {
        OleUninitialize();
        m_oleInitialized = false;
    }
}

bool App::Initialize(HINSTANCE hInstance, int nCmdShow,
                     const std::wstring& startupSessionId,
                     const std::wstring& startupWorkingDir) {
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
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101));  // IDI_WMUX
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(101));

    if (!RegisterClassExW(&wc))
        return false;

    m_hwnd = CreateWindowExW(
        0, L"WmuxWindowClass", L"WMUX",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        m_settings.windowWidth, m_settings.windowHeight,
        nullptr, nullptr, hInstance, this);

    if (!m_hwnd)
        return false;

    if (!m_renderer.Initialize(m_hwnd, m_settings.fontName, m_settings.fontSize,
                                m_settings.backgroundColor))
        return false;
    m_renderer.SetSeparatorColor(m_settings.separatorColor);

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    float statusH = m_renderer.GetStatusBarHeight();
    D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right),
                             static_cast<float>(rc.bottom) - statusH};

    if (!startupSessionId.empty()) {
        if (!m_paneManager.InitializeWithSession(
                paneRect, m_hwnd, WM_PTY_OUTPUT,
                m_renderer.GetCellWidth(), m_renderer.GetCellHeight(),
                AttachPaneSession(startupSessionId)))
            return false;
    } else {
        if (!m_paneManager.Initialize(paneRect, m_hwnd, WM_PTY_OUTPUT,
                                       m_renderer.GetCellWidth(),
                                       m_renderer.GetCellHeight(),
                                       startupWorkingDir))
            return false;
    }

    // Initialize OLE for drag-and-drop
    HRESULT oleResult = OleInitialize(nullptr);
    if (FAILED(oleResult))
        return false;
    m_oleInitialized = true;

    // Register drop target
    m_dropTarget = new DropTarget([this](const std::wstring& path) {
        OnDropFolder(path);
    });
    RegisterDragDrop(m_hwnd, m_dropTarget);

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    // Timer for status bar clock update (1 second)
    SetTimer(m_hwnd, TIMER_CLOCK, 1000, nullptr);
    // Timer for flushing pending input (50ms)
    SetTimer(m_hwnd, TIMER_FLUSH_INPUT, 50, nullptr);
    m_lastUserInputTick = GetTickCount64();

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
    case WM_COPYDATA: {
        auto* copyData = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!copyData)
            break;

        if (copyData->dwData == GetAttachPaneCopyDataId()) {
            m_externalDragPreview = false;
            std::wstring payload;
            if (copyData->lpData && copyData->cbData >= sizeof(wchar_t)) {
                size_t charCount = copyData->cbData / sizeof(wchar_t);
                const auto* text = static_cast<const wchar_t*>(copyData->lpData);
                if (text[charCount - 1] == L'\0')
                    charCount--;
                payload.assign(text, charCount);
            }

            size_t sep = payload.find(L'\n');
            if (sep == std::wstring::npos)
                return FALSE;
            std::wstring header = payload.substr(0, sep);
            std::wstring sessionId = payload.substr(sep + 1);

            int zone = 4;
            size_t comma = header.find(L',');
            if (comma != std::wstring::npos) {
                int screenX = _wtoi(header.substr(0, comma).c_str());
                int screenY = _wtoi(header.substr(comma + 1).c_str());
                POINT pt = {screenX, screenY};
                ScreenToClient(m_hwnd, &pt);

                IPaneSession* targetPane = nullptr;
                D2D1_RECT_F paneRect = {};
                if (m_paneManager.FindPaneAndRectAtPoint(
                        static_cast<float>(pt.x), static_cast<float>(pt.y),
                        targetPane, paneRect)) {
                    m_paneManager.SetActivePane(targetPane);
                    float relX = static_cast<float>(pt.x - paneRect.left) /
                                 (paneRect.right - paneRect.left);
                    float relY = static_cast<float>(pt.y - paneRect.top) /
                                 (paneRect.bottom - paneRect.top);
                    const float edgeSize = 0.25f;
                    if (relY < edgeSize) zone = 0;
                    else if (relY > 1.0f - edgeSize) zone = 2;
                    else if (relX < edgeSize) zone = 3;
                    else if (relX > 1.0f - edgeSize) zone = 1;
                }
            } else {
                zone = _wtoi(header.c_str());
            }
            return AttachExternalSession(sessionId, zone) ? TRUE : FALSE;
        }
        break;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_DPICHANGED:
        OnDpiChanged(HIWORD(wParam), *reinterpret_cast<RECT*>(lParam));
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

    case WM_SYSCOMMAND:
        // Block system menu (Alt+Space and title bar icon click)
        if ((wParam & 0xFFF0) == SC_KEYMENU || (wParam & 0xFFF0) == SC_MOUSEMENU)
            return 0;
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        RegisterUserActivity();
        if (OnKeyDown(wParam, lParam))
            return 0;
        break;

    case WM_CHAR:
        RegisterUserActivity();
        OnChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_IME_STARTCOMPOSITION:
        // Prevent default IME composition window from appearing
        return 0;

    case WM_IME_COMPOSITION:
        // In PREFIX mode, intercept Korean IME composition to handle commands immediately
        if (m_inputMode == InputMode::Prefix && (lParam & GCS_COMPSTR)) {
            HIMC hImc = ImmGetContext(m_hwnd);
            if (hImc) {
                wchar_t buf[8] = {};
                LONG len = ImmGetCompositionStringW(hImc, GCS_COMPSTR, buf, sizeof(buf));
                if (len > 0 && buf[0] != 0) {
                    wchar_t korChar = buf[0];

                    // Map Korean Hangul to English commands
                    wchar_t cmd = 0;
                    switch (korChar) {
                    case 0x3157: cmd = 'h'; break;  // ㅗ -> h (horizontal split)
                    case 0x314F: cmd = 'h'; break;  // ㅗ -> h (horizontal split)
                    case 0x314D: cmd = 'v'; break;  // ㅍ -> v (vertical split)
                    case 0x314C: cmd = 'x'; break;  // ㅌ -> x (close)
                    case 0x314B: cmd = 'z'; break;  // ㅋ -> z (zoom)
                    case 0x3150: cmd = 'o'; break;  // ㅐ -> o (options)
                    }

                    // Cancel IME composition
                    ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                    ImmReleaseContext(m_hwnd, hImc);

                    if (cmd != 0) {
                        // Execute command directly
                        ExitPrefixMode();

                        switch (cmd) {
                        case 'v':
                            SplitActivePane(SplitDirection::Vertical);
                            break;
                        case 'h':
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
                        return 0;
                    }
                }
                ImmReleaseContext(m_hwnd, hImc);
            }
        }

        // In NORMAL mode, show composing character in the pane
        if (m_inputMode == InputMode::Normal && (lParam & GCS_COMPSTR)) {
            HIMC hImc = ImmGetContext(m_hwnd);
            if (hImc) {
                LONG bytes = ImmGetCompositionStringW(hImc, GCS_COMPSTR, nullptr, 0);
                if (bytes > 0) {
                    m_imeCompStr.resize(bytes / sizeof(wchar_t));
                    ImmGetCompositionStringW(hImc, GCS_COMPSTR, &m_imeCompStr[0], bytes);
                    m_imeComposing = true;
                    if (IPaneSession* ap = m_paneManager.GetActivePane())
                        ap->GetBuffer().ScrollToBottom();
                } else {
                    m_imeComposing = false;
                    m_imeCompStr.clear();
                }
                ImmReleaseContext(m_hwnd, hImc);
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        }
        if (lParam & GCS_RESULTSTR) {
            m_imeComposing = false;
            m_imeCompStr.clear();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return DefWindowProc(m_hwnd, msg, wParam, lParam);

    case WM_IME_ENDCOMPOSITION:
        m_imeComposing = false;
        m_imeCompStr.clear();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL:
        RegisterUserActivity();
        OnMouseWheel(wParam, lParam);
        return 0;

    case WM_LBUTTONDOWN:
        RegisterUserActivity();
        OnLButtonDown(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                      static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;

    case WM_LBUTTONDBLCLK:
        RegisterUserActivity();
        OnLButtonDblClk(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                        static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;

    case WM_MOUSEMOVE: {
        RegisterUserActivity();
        int mx = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        int my = static_cast<int>(static_cast<short>(HIWORD(lParam)));

        if (m_draggingPane || m_draggingHelpScrollbar || m_draggingSeparator || m_draggingScrollbar || m_selecting) {
            OnMouseMove(mx, my);
        } else {
            // Update cursor when hovering over help popup or separator or scrollbar
            if (m_showHelp) {
                RECT rc;
                GetClientRect(m_hwnd, &rc);
                float lineHeight = m_renderer.GetCellHeight();
                float visibleHeight = lineHeight * DxRenderer::HELP_VISIBLE_LINES;
                float popupHeight = visibleHeight + DxRenderer::HELP_POPUP_PADDING * 2.0f;
                float left = (static_cast<float>(rc.right) - DxRenderer::HELP_POPUP_WIDTH) * 0.5f;
                float top = (static_cast<float>(rc.bottom) - popupHeight) * 0.5f;
                D2D1_RECT_F popupRect = {left, top, left + DxRenderer::HELP_POPUP_WIDTH, top + popupHeight};

                if (mx >= popupRect.left && mx <= popupRect.right &&
                    my >= popupRect.top && my <= popupRect.bottom) {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }
            } else {
                SplitNode* node = nullptr;
                IPaneSession* pane = nullptr;
                D2D1_RECT_F rect = {};
                if (HitTestSeparator(static_cast<float>(mx), static_cast<float>(my), node)) {
                    if (node->direction == SplitDirection::Vertical)
                        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    else
                        SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                } else if (HitTestScrollbar(static_cast<float>(mx), static_cast<float>(my), pane, rect)) {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                }
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        RegisterUserActivity();
        if (m_draggingPane || m_draggingHelpScrollbar || m_draggingSeparator || m_draggingScrollbar || m_selecting)
            OnLButtonUp();
        return 0;

    case WM_RBUTTONUP:
        RegisterUserActivity();
        OnRButtonUp(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;

    case WM_NCRBUTTONDOWN:
        // Block right-click on title bar (system menu)
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_CLOCK) {
            UpdateIdleScrambleState();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        } else if (wParam == TIMER_PREFIX) {
            ExitPrefixMode();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        } else if (wParam == TIMER_SELECTION_AUTOSCROLL) {
            ContinueSelectionAutoScroll();
        } else if (wParam == TIMER_IDLE_SCRAMBLE) {
            UpdateIdleScrambleState();
            if (m_idleScrambleActive) {
                m_idleScrambleFrame++;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        } else if (wParam == TIMER_FLUSH_INPUT) {
            // Try to flush pending input for all panes
            m_paneManager.ForEachLeaf([](SplitNode& node) {
                if (node.pane) {
                    node.pane->TryFlushPendingInput();
                }
            });
        } else if (wParam == TIMER_RESET_SKIP_FLAG) {
            // Safety reset: if WM_CHAR wasn't delivered after setting m_skipNextChar,
            // force reset to prevent next input from being skipped
            KillTimer(m_hwnd, TIMER_RESET_SKIP_FLAG);
            m_skipNextChar = false;
        }
        return 0;

    case WM_PTY_OUTPUT:
        OnPtyOutput(wParam, lParam);
        return 0;

    case WM_CLOSE: {
        KillTimer(m_hwnd, TIMER_CLOCK);
        KillTimer(m_hwnd, TIMER_PREFIX);
        KillTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL);
        KillTimer(m_hwnd, TIMER_IDLE_SCRAMBLE);
        KillTimer(m_hwnd, TIMER_FLUSH_INPUT);
        KillTimer(m_hwnd, TIMER_RESET_SKIP_FLAG);
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
        if (msg == GetDragPreviewMsg()) {
            if (wParam == 1) {
                m_externalDragPreview = false;
            } else {
                int sx = static_cast<int>(static_cast<short>(LOWORD(lParam)));
                int sy = static_cast<int>(static_cast<short>(HIWORD(lParam)));
                POINT pt = {sx, sy};
                ScreenToClient(m_hwnd, &pt);

                IPaneSession* pane = nullptr;
                D2D1_RECT_F paneRect = {};
                if (m_paneManager.FindPaneAndRectAtPoint(
                        static_cast<float>(pt.x), static_cast<float>(pt.y), pane, paneRect)) {
                    float relX = static_cast<float>(pt.x - paneRect.left) / (paneRect.right - paneRect.left);
                    float relY = static_cast<float>(pt.y - paneRect.top) / (paneRect.bottom - paneRect.top);
                    int zone = 4;
                    const float edgeSize = 0.25f;
                    if (relY < edgeSize) zone = 0;
                    else if (relY > 1.0f - edgeSize) zone = 2;
                    else if (relX < edgeSize) zone = 3;
                    else if (relX > 1.0f - edgeSize) zone = 1;

                    m_externalDragPreview = true;
                    m_externalPreviewRect = paneRect;
                    m_externalPreviewZone = zone;
                } else {
                    m_externalDragPreview = false;
                }
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        return DefWindowProc(m_hwnd, msg, wParam, lParam);
    }

    return DefWindowProc(m_hwnd, msg, wParam, lParam);
}

void App::EnterPrefixMode() {
    m_inputMode = InputMode::Prefix;
    SetTimer(m_hwnd, TIMER_PREFIX, static_cast<UINT>(m_settings.prefixTimeoutMs), nullptr);
    UpdateTitleBar();
}

void App::ExitPrefixMode() {
    if (m_inputMode == InputMode::Prefix)
        m_inputMode = InputMode::Normal;
    KillTimer(m_hwnd, TIMER_PREFIX);
    UpdateTitleBar();
}

void App::SelectAllVisible(IPaneSession* pane) {
    if (!pane) return;
    auto& buf = pane->GetBuffer();
    m_selectPane = pane;
    m_paneManager.ForEachLeaf([&](SplitNode& node) {
        if (node.pane.get() == pane)
            m_selectPaneRect = node.rect;
    });
    m_selStartRow = buf.ViewRowToDocumentRow(0);
    m_selStartCol = 0;
    m_selEndRow = buf.ViewRowToDocumentRow(buf.GetRows() - 1);
    m_selEndCol = buf.GetCols() - 1;
    m_hasSelection = true;
    m_selecting = false;
}

void App::SelectLineAt(IPaneSession* pane, int row) {
    if (!pane) return;
    auto& buf = pane->GetBuffer();
    if (row < 0) row = 0;
    if (row >= buf.GetRows()) row = buf.GetRows() - 1;
    if (row < 0) return;
    int documentRow = buf.ViewRowToDocumentRow(row);

    m_selectPane = pane;
    m_paneManager.ForEachLeaf([&](SplitNode& node) {
        if (node.pane.get() == pane)
            m_selectPaneRect = node.rect;
    });
    m_selStartRow = documentRow;
    m_selEndRow = documentRow;
    m_selStartCol = 0;
    m_selEndCol = buf.GetCols() - 1;
    m_hasSelection = true;
    m_selecting = false;
}

void App::ApplyVisualSettings(const Settings& settings) {
    m_renderer.UpdateFont(settings.fontName, settings.fontSize);
    m_renderer.SetBackgroundColor(settings.backgroundColor);
    m_renderer.SetSeparatorColor(settings.separatorColor);
    m_settings.fontName = settings.fontName;
    m_settings.fontSize = settings.fontSize;
    m_settings.backgroundColor = settings.backgroundColor;
    m_settings.separatorColor = settings.separatorColor;
    m_settings.dimInactivePanes = settings.dimInactivePanes;
    m_settings.showPrefixOverlay = settings.showPrefixOverlay;
    m_settings.idleScrambleMinutes = settings.idleScrambleMinutes;
    UpdateIdleScrambleState();

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    float statusH = m_renderer.GetStatusBarHeight();
    D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right),
                             static_cast<float>(rc.bottom) - statusH};
    m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(),
                           m_renderer.GetCellHeight());
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::RegisterUserActivity() {
    m_lastUserInputTick = GetTickCount64();
    if (m_idleScrambleActive) {
        m_idleScrambleActive = false;
        m_idleScrambleFrame = 0;
        m_renderer.ResetMatrixEffect();
        KillTimer(m_hwnd, TIMER_IDLE_SCRAMBLE);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void App::UpdateIdleScrambleState() {
    bool shouldEnable = false;
    if (m_settings.idleScrambleMinutes > 0) {
        ULONGLONG idleMs = static_cast<ULONGLONG>(m_settings.idleScrambleMinutes) * 60ull * 1000ull;
        shouldEnable = (GetTickCount64() - m_lastUserInputTick) >= idleMs;
    }

    if (shouldEnable == m_idleScrambleActive)
        return;

    m_idleScrambleActive = shouldEnable;
    if (m_idleScrambleActive) {
        m_idleScrambleFrame = 0;
        SetTimer(m_hwnd, TIMER_IDLE_SCRAMBLE, 33, nullptr);
    } else {
        KillTimer(m_hwnd, TIMER_IDLE_SCRAMBLE);
    }
}





void App::UpdateSelectionAutoScroll() {
    if (!m_selecting || !m_selectPane) {
        KillTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL);
        return;
    }

    float padding = DxRenderer::GetPanePadding();
    bool outside = (m_lastMouseY < static_cast<int>(m_selectPaneRect.top + padding)) ||
                   (m_lastMouseY >= static_cast<int>(m_selectPaneRect.bottom - padding));
    if (outside)
        SetTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL, 50, nullptr);
    else
        KillTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL);
}

void App::ContinueSelectionAutoScroll() {
    if (!m_selecting || !m_selectPane) {
        KillTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL);
        return;
    }

    auto& buf = m_selectPane->GetBuffer();
    float padding = DxRenderer::GetPanePadding();
    float cellHeight = (std::max)(1.0f, m_renderer.GetCellHeight());
    bool changed = false;

    if (m_lastMouseY < static_cast<int>(m_selectPaneRect.top + padding)) {
        int lines = 1 + static_cast<int>((m_selectPaneRect.top + padding - m_lastMouseY) / cellHeight);
        buf.ScrollBack(lines);
        changed = true;
    } else if (m_lastMouseY >= static_cast<int>(m_selectPaneRect.bottom - padding)) {
        int lines = 1 + static_cast<int>((m_lastMouseY - (m_selectPaneRect.bottom - padding)) / cellHeight);
        buf.ScrollForward(lines);
        changed = true;
    } else {
        KillTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL);
    }

    int viewRow = 0;
    MouseToCell(m_lastMouseX, m_lastMouseY, m_selectPane, m_selectPaneRect, viewRow, m_selEndCol);
    m_selEndRow = buf.ViewRowToDocumentRow(viewRow);
    if (changed)
        InvalidateRect(m_hwnd, nullptr, FALSE);
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

            DxRenderer::ImeComposition imeComp{m_imeComposing, m_imeCompStr};
            const DxRenderer::ImeComposition* pIme = m_imeComposing ? &imeComp : nullptr;

            m_renderer.RenderPane(node->pane->GetBuffer(), fullRect, true, true, dragging, pSel,
                                  m_settings.dimInactivePanes, pIme);
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

            DxRenderer::ImeComposition imeComp{m_imeComposing, m_imeCompStr};
            const DxRenderer::ImeComposition* pIme = (isActive && m_imeComposing) ? &imeComp : nullptr;

            m_renderer.RenderPane(node.pane->GetBuffer(), node.rect, isActive, false, dragging, pSel,
                                  m_settings.dimInactivePanes, pIme);
        });

        std::vector<PaneManager::SeparatorLine> seps;
        m_paneManager.CollectSeparators(seps);
        for (auto& s : seps)
            m_renderer.RenderSeparator(s.x1, s.y1, s.x2, s.y2);
    }

    // Matrix rain overlay on top of pane content
    if (m_idleScrambleActive) {
        m_renderer.RenderMatrixEffect(m_idleScrambleFrame);
    }

    if (IPaneSession* active = m_paneManager.GetActivePane()) {
        static ULONGLONG s_lastPaintLogTick = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastPaintLogTick >= 500) {
            s_lastPaintLogTick = now;
            std::wstringstream ss;
            ss << L"OnPaint active rows=" << active->GetBuffer().GetRows()
               << L" cols=" << active->GetBuffer().GetCols()
               << L" visibleCells=" << CountVisibleCells(active->GetBuffer())
               << L" ready=" << (active->IsReady() ? 1 : 0)
               << L" running=" << (active->IsRunning() ? 1 : 0);
            AppendRuntimeLog(ss.str());
        }
    }

    // Status bar
    std::wstring statusLeft;
    std::wstring statusRight;
    int paneCount = 0;
    int currentPaneIndex = 0;
    int index = 0;
    IPaneSession* active = m_paneManager.GetActivePane();

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

        // Get working directory and abbreviate if too long
        std::wstring wdir = active->GetWorkingDirectory();
        if (!wdir.empty()) {
            // Abbreviate path if longer than 40 characters
            if (wdir.length() > 40) {
                // Show last 2 path components (e.g., ...\parent\current)
                size_t lastSep = wdir.rfind(L'\\');
                if (lastSep != std::wstring::npos && lastSep > 0) {
                    size_t secondLastSep = wdir.rfind(L'\\', lastSep - 1);
                    if (secondLastSep != std::wstring::npos) {
                        wdir = L"...\\" + wdir.substr(secondLastSep + 1);
                    }
                }
            }
        }

        // Time
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t timeBuf[16];
        wsprintfW(timeBuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

        statusLeft = L" [" + std::to_wstring(currentPaneIndex) + L"/" + std::to_wstring(paneCount) + L"] " + wtitle;
        if (!wdir.empty()) {
            statusLeft += L" | " + wdir;
        }
        if (m_paneManager.IsZoomed()) statusLeft += L" [ZOOM]";
        statusRight = timeBuf;
        if (m_inputMode == InputMode::Prefix)
            statusRight = L"PREFIX  " + statusRight;
        if (m_idleScrambleActive)
            statusRight = L"IDLE  " + statusRight;
    }

    m_renderer.RenderStatusBar(paneAreaH, clientW, statusLeft, statusRight, m_paneManager.IsZoomed());

    // Draw orange border when in zoom mode
    if (m_paneManager.IsZoomed()) {
        m_renderer.RenderZoomBorder(clientW, paneAreaH);
    }

    if (m_inputMode == InputMode::Prefix && m_settings.showPrefixOverlay) {
        m_renderer.RenderPrefixIndicator();
        m_renderer.RenderPrefixOverlay(L"V vertical  H horizontal  X close  Z zoom  O settings  Esc cancel");
    }

    // Render drop zone preview when dragging pane
    if (m_draggingPane && m_dropTargetPane && m_dropZone >= 0) {
        D2D1_RECT_F targetRect = {};
        m_paneManager.ForEachLeaf([&](SplitNode& node) {
            if (node.pane.get() == m_dropTargetPane)
                targetRect = node.rect;
        });
        m_renderer.RenderDropZone(targetRect, m_dropZone);
    } else if (m_draggingPane && m_externalDropTarget && m_draggedNode) {
        m_renderer.RenderDropZone(m_draggedNode->rect, 4);
    }

    // Render drop zone preview from external drag (another wmux dragging toward us)
    if (m_externalDragPreview && m_externalPreviewZone >= 0) {
        m_renderer.RenderDropZone(m_externalPreviewRect, m_externalPreviewZone);
    }

    // Render help popup if showing
    if (m_showHelp) {
        m_renderer.RenderHelpPopup(m_helpScrollOffset);
    }

    // Render resume prompt if showing
    if (m_showResumePrompt) {
        SplitNode* activeNode = m_paneManager.GetActiveNode();
        D2D1_RECT_F promptRect = {0, 0, clientW, paneAreaH};
        if (activeNode && !m_paneManager.IsZoomed())
            promptRect = activeNode->rect;
        m_renderer.RenderResumePrompt(promptRect, m_resumeAgentName, m_resumeCommand);
    }

    m_renderer.EndFrame();
}

void App::RelayoutPanes() {
    RECT rc;
    GetClientRect(m_hwnd, &rc);

    float statusH = m_renderer.GetStatusBarHeight();
    float paneH = static_cast<float>(rc.bottom) - statusH;
    if (paneH < 1.0f)
        paneH = 1.0f;

    D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right), paneH};
    m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(),
                           m_renderer.GetCellHeight());
}

void App::OnResize(UINT width, UINT height) {
    m_renderer.Resize(width, height);
    RelayoutPanes();
}

void App::OnDpiChanged(UINT dpi, const RECT& suggestedRect) {
    SetWindowPos(m_hwnd, nullptr,
                 suggestedRect.left, suggestedRect.top,
                 suggestedRect.right - suggestedRect.left,
                 suggestedRect.bottom - suggestedRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    m_renderer.SetDpi(dpi);
    CancelMouseOperation();
    RelayoutPanes();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool App::OnKeyDown(WPARAM vk, LPARAM flags) {
    UINT key = NormalizeKey(vk, flags);
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

    // Alt+H: Toggle help popup (always allowed)
    if (alt && key == 'H') {
        m_showHelp = !m_showHelp;
        m_helpScrollOffset = 0;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }

    // ESC: Close help popup if showing
    if (key == VK_ESCAPE && m_showHelp) {
        m_showHelp = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }

    // Block all other keyboard input when help is showing
    if (m_showHelp) {
        return true;
    }

    // Resume prompt: intercept ESC and R/N keys
    if (m_showResumePrompt) {
        if (key == VK_ESCAPE) {
            HandleResumeChoice(0x1B);
            return true;
        }
        if (key == 'R' || key == 'N') {
            HandleResumeChoice(static_cast<wchar_t>(key));
            m_skipNextChar = true;
            SetTimer(m_hwnd, TIMER_RESET_SKIP_FLAG, 50, nullptr);
            return true;
        }
        return true;
    }

    // Alt+Arrow: Move pane in direction (tree restructuring)
    // Alt+Shift+Arrow: Swap pane content with neighbor
    if (alt && (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT)) {
        if (m_paneManager.IsZoomed()) return true; // No movement in zoom mode

        SplitDirection dir = (key == VK_LEFT || key == VK_RIGHT)
            ? SplitDirection::Vertical : SplitDirection::Horizontal;
        bool forward = (key == VK_RIGHT || key == VK_DOWN);

        if (shift) {
            // Alt+Shift+Arrow: Swap content
            m_paneManager.SwapPaneContent(dir, forward);
        } else {
            // Alt+Arrow: Move pane (requires relayout after tree restructuring)
            m_paneManager.MovePane(dir, forward);
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            float statusH = m_renderer.GetStatusBarHeight();
            float paneH = static_cast<float>(rc.bottom) - statusH;
            D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right), paneH};
            m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(),
                                   m_renderer.GetCellHeight());
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }

    // Ctrl+B: enter prefix mode (check FIRST, before any other logic)
    if (m_inputMode == InputMode::Normal && ctrl && key == 'B') {
        EnterPrefixMode();
        m_skipNextChar = true;
        SetTimer(m_hwnd, TIMER_RESET_SKIP_FLAG, 50, nullptr);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }

    // Prefix mode: handle all commands here (bypasses IME for Korean input)
    if (m_inputMode == InputMode::Prefix) {
        if (key == VK_ESCAPE) {
            ExitPrefixMode();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return true;
        }
        // If VK_PROCESSKEY, IME is active - defer to OnChar for Korean compatibility
        if (key == VK_PROCESSKEY) {
            return true;
        }

        bool handled = true;

        // Arrow keys for pane navigation
        if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
            switch (key) {
            case VK_UP:    m_paneManager.MoveFocus(SplitDirection::Horizontal, false); break;
            case VK_DOWN:  m_paneManager.MoveFocus(SplitDirection::Horizontal, true);  break;
            case VK_LEFT:  m_paneManager.MoveFocus(SplitDirection::Vertical, false);   break;
            case VK_RIGHT: m_paneManager.MoveFocus(SplitDirection::Vertical, true);    break;
            }
        }
        // Character commands (only in English mode - Korean deferred to OnChar)
        else if (key == 'V' || key == '5') {  // v or % (both work)
            SplitActivePane(SplitDirection::Vertical);
        }
        else if (key == 'H' || key == '2') {  // h or " (both work)
            SplitActivePane(SplitDirection::Horizontal);
        }
        else if (key == 'X') {
            CloseActivePane();
        }
        else if (key == 'Z') {
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
        else if (key == 'O') {
            OpenSettings();
        }
        else if (key == 'B' && ctrl) {  // Ctrl+B: send literal 0x02
            if (auto* p = m_paneManager.GetActivePane()) {
                p->SendInput("\x02", 1);
            }
        }
        else {
            handled = false;
        }

        if (handled) {
            ExitPrefixMode();
            m_skipNextChar = true;
            SetTimer(m_hwnd, TIMER_RESET_SKIP_FLAG, 50, nullptr);
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return true;
    }

    // Normal mode
    IPaneSession* active = m_paneManager.GetActivePane();
    if (!active) return false;

    // Input will be buffered if pane is not ready

    // Ctrl+C: copy if selection exists, otherwise send SIGINT (0x03) via WM_CHAR
    if (ctrl && key == 'C' && m_hasSelection) {
        CopySelection();
        m_skipNextChar = true;
        SetTimer(m_hwnd, TIMER_RESET_SKIP_FLAG, 50, nullptr);
        return true;
    }
    // Ctrl+A: select all visible content in active pane
    if (ctrl && key == 'A') {
        SelectAllVisible(active);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }
    // Ctrl+V: paste (only without Shift)
    if (ctrl && !shift && key == 'V') {
        PasteClipboard();
        return true;
    }

    // Ctrl+Shift+D/H/V/X/Z/O/Arrow: direct commands (no prefix needed)
    if (ctrl && shift) {
        bool handled = false;
        if (key == 'H' || key == VK_DOWN) {
            SplitActivePane(SplitDirection::Horizontal);
            handled = true;
        } else if (key == 'V' || key == VK_RIGHT) {
            SplitActivePane(SplitDirection::Vertical);
            handled = true;
        } else if (key == 'X') {
            CloseActivePane();
            handled = true;
        } else if (key == 'D') {
            DetachActivePaneToNewWindow();
            handled = true;
        } else if (key == 'R') {
            OpenDetachablePaneWindow();
            handled = true;
        } else if (key == 'Z') {
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
            handled = true;
        } else if (key == 'O') {
            OpenSettings();
            handled = true;
        }
        if (handled) {
            m_skipNextChar = true;
            SetTimer(m_hwnd, TIMER_RESET_SKIP_FLAG, 50, nullptr);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return true;
        }
    }

    // Shift+Arrow: keyboard text selection (Home/End pass through to shell)
    if (shift && (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
                  key == VK_HOME || key == VK_END)) {
        auto& buf = active->GetBuffer();
        IPaneSession* pane = active;
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
            m_selStartRow = buf.GetScrollbackSize() + buf.GetCursorRow();
            m_selStartCol = buf.GetCursorCol();
            m_selEndRow = m_selStartRow;
            m_selEndCol = m_selStartCol;
            m_hasSelection = true;
        }

        // Extend selection end
        int maxDocumentRow = buf.GetDocumentRowCount() - 1;
        switch (key) {
        case VK_LEFT:
            if (ctrl) {
                // Skip current word class, then stop
                if (m_selEndCol > 0) {
                    const Cell& cur = buf.CellAtDocumentRow(m_selEndRow, m_selEndCol - 1);
                    int cls = CharClass(cur.ch);
                    while (m_selEndCol > 0) {
                        const Cell& c = buf.CellAtDocumentRow(m_selEndRow, m_selEndCol - 1);
                        if (CharClass(c.ch) != cls) break;
                        m_selEndCol--;
                    }
                }
            } else {
                if (m_selEndCol > 0) m_selEndCol--;
                else if (m_selEndRow > 0) { m_selEndRow--; m_selEndCol = buf.GetCols() - 1; }
            }
            break;
        case VK_RIGHT:
            if (ctrl) {
                if (m_selEndCol < buf.GetCols() - 1) {
                    const Cell& cur = buf.CellAtDocumentRow(m_selEndRow, m_selEndCol + 1);
                    int cls = CharClass(cur.ch);
                    while (m_selEndCol < buf.GetCols() - 1) {
                        const Cell& c = buf.CellAtDocumentRow(m_selEndRow, m_selEndCol + 1);
                        if (CharClass(c.ch) != cls) break;
                        m_selEndCol++;
                    }
                }
            } else {
                if (m_selEndCol < buf.GetCols() - 1) m_selEndCol++;
                else if (m_selEndRow < maxDocumentRow) { m_selEndRow++; m_selEndCol = 0; }
            }
            break;
        case VK_UP:
            if (ctrl) m_selEndRow = 0;
            else if (m_selEndRow > 0) m_selEndRow--;
            break;
        case VK_DOWN:
            if (ctrl) m_selEndRow = maxDocumentRow;
            else if (m_selEndRow < maxDocumentRow) m_selEndRow++;
            break;
        case VK_HOME:
            m_selEndCol = 0;
            if (ctrl) m_selEndRow = 0;
            break;
        case VK_END:
            m_selEndCol = buf.GetCols() - 1;
            if (ctrl) m_selEndRow = maxDocumentRow;
            break;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }

    // Any non-shift arrow key clears keyboard selection
    if (!shift && m_hasSelection && !m_selecting &&
        (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)) {
        ClearSelection();
    }

    // Shift+PageUp/Down: scrollback navigation
    if (shift && (key == VK_PRIOR || key == VK_NEXT)) {
        int pageSize = active->GetBuffer().GetRows() / 2;
        if (pageSize < 1) pageSize = 1;
        if (key == VK_PRIOR)
            active->GetBuffer().ScrollBack(pageSize);
        else
            active->GetBuffer().ScrollForward(pageSize);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }

    // Ctrl+Arrow: pane focus navigation
    if (ctrl && (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT)) {
        switch (key) {
        case VK_UP:    m_paneManager.MoveFocus(SplitDirection::Horizontal, false); break;
        case VK_DOWN:  m_paneManager.MoveFocus(SplitDirection::Horizontal, true);  break;
        case VK_LEFT:  m_paneManager.MoveFocus(SplitDirection::Vertical, false);   break;
        case VK_RIGHT: m_paneManager.MoveFocus(SplitDirection::Vertical, true);    break;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }

    // Send VT sequences to active pane
    std::string seq;
    bool appCur = active->GetBuffer().IsAppCursorKeys();

    switch (key) {
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
    default: return false;
    }

    // SendInput will buffer if not ready
    active->SendInput(seq);
    return true;
}

void App::OnChar(wchar_t ch) {
    // Skip character if already handled in OnKeyDown
    if (m_skipNextChar) {
        m_skipNextChar = false;
        KillTimer(m_hwnd, TIMER_RESET_SKIP_FLAG);
        return;
    }

    // Block all character input when help is showing
    if (m_showHelp) {
        return;
    }

    // Resume prompt intercept
    if (m_showResumePrompt) {
        HandleResumeChoice(ch);
        return;
    }

    // Ctrl+Shift combinations are wmux commands - never send to console
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && shift) {
        return;
    }

    // Prefix key detection (fallback if OnKeyDown missed it)
    if (m_inputMode == InputMode::Normal) {
        if (ch == 0x02) { // Ctrl+B: enter prefix mode
            EnterPrefixMode();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        // Ctrl+V (0x16) handled in OnKeyDown
        if (ch == 0x16) return;
        // Ctrl+C (0x03): handled in OnKeyDown (copy or SIGINT)
        if (ch == 0x03) {
            m_currentInputLine.clear();
            if (!m_hasSelection) {
                IPaneSession* active = m_paneManager.GetActivePane();
                if (active) {
                    active->SendInput("\x03", 1);
                }
            }
            return;
        }
        // Ctrl+A (0x01): handled in OnKeyDown (select all)
        if (ch == 0x01) return;
        // Ctrl+C clears input tracking
        // (already handled above, but also reset tracked line)
        // Backspace: send DEL (0x7F) instead of BS (0x08)
        if (ch == 0x08) {
            if (!m_currentInputLine.empty())
                m_currentInputLine.pop_back();
            IPaneSession* active = m_paneManager.GetActivePane();
            if (active) {
                active->GetBuffer().ScrollToBottom();
                active->SendInput("\x7f", 1);
            }
            return;
        }

        IPaneSession* active = m_paneManager.GetActivePane();
        if (!active) return;

        // Enter: check for agent launch before sending
        if (ch == 0x0D) {
            std::wstring agentName;
            if (ResumeManager::DetectAgentLaunch(m_currentInputLine, agentName)) {
                std::wstring cwd = active->GetWorkingDirectory();
                ResumeManager::ResumeEntry entry;
                if (!cwd.empty() && m_resumeManager.LoadResume(cwd, agentName, entry)) {
                    m_showResumePrompt = true;
                    m_resumeAgentName = agentName;
                    m_resumeCommand = entry.resumeCmd;
                    m_pendingUserCommand = m_currentInputLine;
                    m_currentInputLine.clear();
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }
            }
            m_currentInputLine.clear();
            active->GetBuffer().ScrollToBottom();
            active->SendInput("\r", 1);
            return;
        }

        // Track printable input
        if (ch >= 0x20)
            m_currentInputLine += ch;

        // Auto-scroll to bottom on typing (but not for control characters)
        active->GetBuffer().ScrollToBottom();

        char utf8[4];
        int len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, sizeof(utf8),
                                      nullptr, nullptr);
        if (len > 0)
            active->SendInput(utf8, len);
        return;
    }

    // Prefix mode commands
    ExitPrefixMode();

    // Map Korean Hangul characters to English commands (for IME compatibility)
    wchar_t cmd = ch;
    switch (ch) {
    case 0x3157:
    case 0x314F:  // ㅗ -> h
        cmd = 'h';
        break;
    case 0x314D:  // ㅍ -> v
        cmd = 'v';
        break;
    case 0x314C:  // ㅌ -> x
        cmd = 'x';
        break;
    case 0x314B:  // ㅋ -> z
        cmd = 'z';
        break;
    case 0x3150:  // ㅐ -> o
        cmd = 'o';
        break;
    }

    switch (cmd) {
    case 0x02: // Double Ctrl+B: send literal to pane
        if (auto* p = m_paneManager.GetActivePane()) {
            p->SendInput("\x02", 1);
        }
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
    IPaneSession* pane = m_paneManager.FindPaneById(paneId);
    if (!pane) return;

    if (wParam == 1) {
        if (m_selectPane == pane)
            ClearSelection();
        if (m_dragPane == pane)
            m_dragPane = nullptr;
        if (m_draggedPane == pane)
            m_draggedPane = nullptr;
        if (m_dropTargetPane == pane)
            m_dropTargetPane = nullptr;
        if (m_lastDblClickPane == pane)
            m_lastDblClickPane = nullptr;
        

        if (!m_paneManager.ClosePaneById(paneId)) {
            PostMessage(m_hwnd, WM_CLOSE, 0, 0);
            return;
        }
        RelayoutAfterPaneRemoval();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    pane->ProcessOutput();

    // Scan for resume commands in output
    {
        std::wstring agentName, resumeCmd;
        if (ResumeManager::ScanForResumeCommand(pane->GetBuffer(), agentName, resumeCmd)) {
            std::wstring cwd = pane->GetWorkingDirectory();
            if (!cwd.empty())
                m_resumeManager.SaveResume(cwd, agentName, resumeCmd);
        }
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::OnMouseWheel(WPARAM wParam, LPARAM lParam) {
    short delta = static_cast<short>(HIWORD(wParam));

    // If help popup is showing, scroll the help content
    if (m_showHelp) {
        int steps = delta / WHEEL_DELTA;
        m_helpScrollOffset -= steps * 3;

        const int maxScroll = (std::max)(0, DxRenderer::GetHelpLineCount() - DxRenderer::HELP_VISIBLE_LINES);
        if (m_helpScrollOffset < 0) m_helpScrollOffset = 0;
        if (m_helpScrollOffset > maxScroll) m_helpScrollOffset = maxScroll;

        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Screen coords → client coords
    POINT pt;
    pt.x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
    pt.y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
    ScreenToClient(m_hwnd, &pt);

    IPaneSession* pane = m_paneManager.FindPaneAtPoint(
        static_cast<float>(pt.x), static_cast<float>(pt.y));
    if (!pane)
        pane = m_paneManager.GetActivePane();
    if (!pane) return;

    UINT linesPerNotch = 3;
    if (m_settings.scrollLines > 0) {
        linesPerNotch = static_cast<UINT>(m_settings.scrollLines);
    } else {
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0);
    }

    int totalDelta = m_wheelDeltaRemainder + delta;
    int steps = totalDelta / WHEEL_DELTA;
    m_wheelDeltaRemainder = totalDelta - steps * WHEEL_DELTA;

    if (steps == 0 || linesPerNotch == 0)
        return;

    int lines = 0;
    if (linesPerNotch == WHEEL_PAGESCROLL) {
        lines = (std::max)(1, pane->GetBuffer().GetRows() - 1);
    } else {
        lines = static_cast<int>(linesPerNotch) * (steps > 0 ? steps : -steps);
    }

    if (steps > 0)
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

void App::OpenDetachablePaneWindow() {
    PaneDescriptor activeDesc = DescribeActivePane();
    std::wstring workingDir = activeDesc.workingDirectory;

    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"";
    command += exePath;
    command += L"\" --new-window";
    if (!workingDir.empty()) {
        command += L" --cwd ";
        AppendQuotedArg(command, workingDir);
    }
    std::vector<wchar_t> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi))
        return;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

PaneDescriptor App::DescribeActivePane() const {
    if (const IPaneSession* active = m_paneManager.GetActivePane())
        return active->Describe();
    return {};
}

static void AppendQuotedArg(std::wstring& command, const std::wstring& arg) {
    command += L"\"";
    for (wchar_t ch : arg) {
        if (ch == L'"')
            command += L'\\';
        command += ch;
    }
    command += L"\"";
}

bool App::AttachExternalSession(const std::wstring& sessionId, int zone) {
    if (sessionId.empty())
        return false;

    if (zone == 4)
        zone = 1;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    float statusH = m_renderer.GetStatusBarHeight();
    float paneH = static_cast<float>(rc.bottom) - statusH;
    D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right), paneH};
    int cols = m_renderer.CalcCols(rc.right);
    int rows = m_renderer.CalcRows(static_cast<UINT>(paneH));

    uint32_t paneId = m_paneManager.AllocatePaneId();
    auto pane = AttachPaneSession(sessionId);
    if (!pane || !pane->Start(cols, rows, m_hwnd, WM_PTY_OUTPUT, L"", paneId))
        return false;

    if (!m_paneManager.AttachToActive(std::move(pane), m_hwnd, WM_PTY_OUTPUT,
                                      m_renderer.GetCellWidth(),
                                      m_renderer.GetCellHeight(),
                                      paneId, zone)) {
        return false;
    }

    m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(), m_renderer.GetCellHeight());
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return true;
}

void App::RelayoutAfterPaneRemoval() {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    float statusH = m_renderer.GetStatusBarHeight();
    D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right),
                             static_cast<float>(rc.bottom) - statusH};
    m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(),
                           m_renderer.GetCellHeight());
}

void App::CloseActivePane() {
    // Don't close if only one pane remains
    if (m_paneManager.HasSinglePane())
        return;

    if (!m_paneManager.CloseActive()) {
        // Last pane closed (should not happen due to HasSinglePane check)
        DestroyWindow(m_hwnd);
        return;
    }

    RelayoutAfterPaneRemoval();
}

void App::DetachActivePaneToNewWindow() {
    if (m_paneManager.HasSinglePane())
        return;
    IPaneSession* active = m_paneManager.GetActivePane();
    if (!active)
        return;
    bool wasSinglePane = m_paneManager.HasSinglePane();

    std::wstring sessionToken = active->GetSessionToken();
    if (sessionToken.empty())
        return;

    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return;

    if (m_selectPane == active)
        ClearSelection();
    if (m_dragPane == active)
        m_dragPane = nullptr;
    if (m_draggedPane == active)
        m_draggedPane = nullptr;
    if (m_dropTargetPane == active)
        m_dropTargetPane = nullptr;
    if (m_lastDblClickPane == active)
        m_lastDblClickPane = nullptr;
    

    active->PrepareForMove();
    active->Stop();

    std::wstring command = L"\"";
    command += exePath;
    command += L"\" --attach-session \"";
    command += sessionToken;
    command += L"\" --new-window";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        m_paneManager.RemoveActiveWithoutStopping();
        if (wasSinglePane) {
            DestroyWindow(m_hwnd);
            return;
        }
        RelayoutAfterPaneRemoval();
        return;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    m_paneManager.RemoveActiveWithoutStopping();

    if (wasSinglePane) {
        DestroyWindow(m_hwnd);
        return;
    }

    RelayoutAfterPaneRemoval();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool App::HitTestScrollbar(float px, float py, IPaneSession*& outPane, D2D1_RECT_F& outRect) {
    if (!m_paneManager.FindPaneAndRectAtPoint(px, py, outPane, outRect))
        return false;

    int sbSize = outPane->GetBuffer().GetScrollbackSize();
    if (sbSize <= 0) return false;

    float padding = DxRenderer::GetPanePadding();
    float barW = 12.0f;
    float barX = outRect.right - padding - barW;
    float barRight = outRect.right - padding;
    return px >= barX && px <= barRight &&
           py >= outRect.top + padding && py <= outRect.bottom - padding;
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

void App::MouseToCell(int mx, int my, IPaneSession* pane, D2D1_RECT_F rect, int& row, int& col) {
    float cw = m_renderer.GetCellWidth();
    float ch = m_renderer.GetCellHeight();
    float padding = DxRenderer::GetPanePadding();
    float contentLeft = rect.left + padding;
    float contentTop = rect.top + padding;
    float contentRight = rect.right - padding;
    float contentBottom = rect.bottom - padding;

    float clampedX = static_cast<float>(mx);
    float clampedY = static_cast<float>(my);
    if (clampedX < contentLeft) clampedX = contentLeft;
    if (clampedY < contentTop) clampedY = contentTop;
    if (clampedX >= contentRight) clampedX = contentRight - 1.0f;
    if (clampedY >= contentBottom) clampedY = contentBottom - 1.0f;

    col = static_cast<int>((clampedX - contentLeft) / cw);
    row = static_cast<int>((clampedY - contentTop) / ch);
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (pane) {
        auto& buf = pane->GetBuffer();
        if (col >= buf.GetCols()) col = buf.GetCols() - 1;
        if (row >= buf.GetRows()) row = buf.GetRows() - 1;
    }
}

// 0=whitespace/control, 1=ASCII word, 2=Hangul, 3=CJK, 4=delimiter/symbol
static int CharClass(wchar_t ch) {
    if (ch <= L' ') return 0;
    if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
        (ch >= L'0' && ch <= L'9') || ch == L'_')
        return 1;
    // Hangul Jamo + Compatibility Jamo + Syllables + Extended
    if ((ch >= 0x1100 && ch <= 0x11FF) ||
        (ch >= 0x3130 && ch <= 0x318F) ||
        (ch >= 0xAC00 && ch <= 0xD7A3) ||
        (ch >= 0xA960 && ch <= 0xA97C) ||
        (ch >= 0xD7B0 && ch <= 0xD7FB))
        return 2;
    // CJK Unified Ideographs + Extension A + Radicals
    if ((ch >= 0x2E80 && ch <= 0x2FFF) ||
        (ch >= 0x3400 && ch <= 0x4DBF) ||
        (ch >= 0x4E00 && ch <= 0x9FFF) ||
        (ch >= 0xF900 && ch <= 0xFAFF))
        return 3;
    // Fullwidth Latin
    if (ch >= 0xFF01 && ch <= 0xFF5E) return 1;
    // Fullwidth Hangul
    if (ch >= 0xFFA0 && ch <= 0xFFDC) return 2;
    // Common delimiters
    if (ch == L'(' || ch == L')' || ch == L'[' || ch == L']' ||
        ch == L'{' || ch == L'}' || ch == L'<' || ch == L'>' ||
        ch == L'"' || ch == L'\'' || ch == L',' || ch == L';' || ch == L'`')
        return 4;
    // Other ASCII symbols: treat as word to allow path/URL selection
    return 1;
}

static bool IsWordChar(wchar_t ch) {
    return CharClass(ch) != 0 && CharClass(ch) != 4;
}

void App::OnLButtonDblClk(int x, int y) {
    // Block double-click when help is showing
    if (m_showHelp) {
        return;
    }

    ClearSelection();
    IPaneSession* pane = nullptr;
    D2D1_RECT_F rect = {};
    if (!m_paneManager.FindPaneAndRectAtPoint(
            static_cast<float>(x), static_cast<float>(y), pane, rect))
        return;

    int row, col;
    MouseToCell(x, y, pane, rect, row, col);

    DWORD now = GetTickCount();
    if (pane == m_lastDblClickPane &&
        row == m_lastDblClickRow &&
        now - m_lastDblClickTick <= GetDoubleClickTime()) {
        SelectLineAt(pane, row);
        m_lastDblClickTick = 0;
        m_lastDblClickRow = -1;
        m_lastDblClickPane = nullptr;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    m_lastDblClickTick = now;
    m_lastDblClickRow = row;
    m_lastDblClickPane = pane;

    auto& buf = pane->GetBuffer();
    int cols = buf.GetCols();
    if (row >= buf.GetRows() || col >= cols) return;

    bool useView = (buf.GetScrollOffset() > 0);
    auto cellAt = [&](int r, int c) -> const Cell& {
        return useView ? buf.ViewAt(r, c) : buf.At(r, c);
    };

    // Check if clicked on a URL — open it and select the URL range
    auto rowCharAt = [&](int c2) -> wchar_t {
        if (c2 < 0 || c2 >= cols) return 0;
        return cellAt(row, c2).ch;
    };
    auto urlSpans = DetectUrls(cols, rowCharAt);
    for (auto& u : urlSpans) {
        if (col >= u.startCol && col <= u.endCol) {
            std::wstring url = ExtractUrlString(u.startCol, u.endCol, rowCharAt);
            if (!url.empty())
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

            m_selectPane = pane;
            m_selectPaneRect = rect;
            int documentRow = buf.ViewRowToDocumentRow(row);
            m_selStartRow = documentRow;
            m_selStartCol = u.startCol;
            m_selEndRow = documentRow;
            m_selEndCol = u.endCol;
            m_hasSelection = true;
            m_selecting = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
    }

    // If clicked on trail half of wide char, snap to lead cell
    if (cellAt(row, col).width == 0 && col > 0)
        col--;

    const Cell& clicked = cellAt(row, col);
    int cls = CharClass(clicked.ch);
    if (cls == 0 || cls == 4) return;

    // Expand left (same character class)
    int left = col;
    while (left > 0) {
        int prev = left - 1;
        const Cell& c = cellAt(row, prev);
        if (c.width == 0 && prev > 0) prev--;
        if (CharClass(cellAt(row, prev).ch) != cls) break;
        left = prev;
    }
    // Expand right (same character class)
    int right = col;
    if (clicked.width == 2) right++;
    while (right < cols - 1) {
        int next = right + 1;
        const Cell& c = cellAt(row, next);
        if (c.width == 0) { right = next; continue; }
        if (CharClass(c.ch) != cls) break;
        right = next;
        if (c.width == 2 && right + 1 < cols) right++;
    }

    m_selectPane = pane;
    m_selectPaneRect = rect;
    int documentRow = buf.ViewRowToDocumentRow(row);
    m_selStartRow = documentRow;
    m_selStartCol = left;
    m_selEndRow = documentRow;
    m_selEndCol = right;
    m_hasSelection = true;
    m_selecting = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::OnLButtonDown(int x, int y) {
    m_lastMouseX = x;
    m_lastMouseY = y;

    // Help popup handling (highest priority when showing)
    if (m_showHelp) {
        RECT rc;
        GetClientRect(m_hwnd, &rc);

        float lineHeight = m_renderer.GetCellHeight();
        float visibleHeight = lineHeight * DxRenderer::HELP_VISIBLE_LINES;
        float popupHeight = visibleHeight + DxRenderer::HELP_POPUP_PADDING * 2.0f;
        float left = (static_cast<float>(rc.right) - DxRenderer::HELP_POPUP_WIDTH) * 0.5f;
        float top = (static_cast<float>(rc.bottom) - popupHeight) * 0.5f;

        D2D1_RECT_F popupRect = {left, top, left + DxRenderer::HELP_POPUP_WIDTH, top + popupHeight};

        // Check if click is inside popup
        if (x >= popupRect.left && x <= popupRect.right &&
            y >= popupRect.top && y <= popupRect.bottom) {

            // Start dragging anywhere inside popup to scroll
            m_draggingHelpScrollbar = true;
            m_helpDragStartY = static_cast<float>(y);
            m_helpScrollOffsetAtDragStart = m_helpScrollOffset;
            SetCapture(m_hwnd);
            return;
        } else {
            // Click outside popup - close help
            m_showHelp = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
    }

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
    IPaneSession* pane = nullptr;
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

    // Check if Shift is pressed for pane dragging
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    // Pane activation and text selection (or pane drag if Shift pressed)
    ClearSelection();
    if (m_paneManager.FindPaneAndRectAtPoint(
            static_cast<float>(x), static_cast<float>(y), pane, rect)) {
        // Activate clicked pane
        m_paneManager.SetActivePane(pane);

        if (shift) {
            // Start pane drag
            m_draggingPane = true;
            m_draggedPane = pane;
            m_draggedNode = nullptr;
            m_paneManager.ForEachLeaf([&](SplitNode& node) {
                if (node.pane.get() == pane)
                    m_draggedNode = &node;
            });
            SetCapture(m_hwnd);
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        } else {
            // Start text selection
            m_selecting = true;
            m_selectPane = pane;
            m_selectPaneRect = rect;
            int viewRow = 0;
            MouseToCell(x, y, pane, rect, viewRow, m_selStartCol);
            m_selStartRow = pane->GetBuffer().ViewRowToDocumentRow(viewRow);
            m_selEndRow = m_selStartRow;
            m_selEndCol = m_selStartCol;
            SetCapture(m_hwnd);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void App::OnMouseMove(int x, int y) {
    m_lastMouseX = x;
    m_lastMouseY = y;
    if (m_draggingPane) {
        RECT clientRect;
        GetClientRect(m_hwnd, &clientRect);
        bool insideWindow = (x >= 0 && y >= 0 &&
                             x < clientRect.right && y < clientRect.bottom);

        POINT screenPt = {x, y};
        ClientToScreen(m_hwnd, &screenPt);

        HWND newExternalTarget = nullptr;
        if (!insideWindow) {
            HWND targetHwnd = WindowFromPoint(screenPt);
            if (targetHwnd && targetHwnd != m_hwnd) {
                wchar_t className[64] = {};
                GetClassNameW(targetHwnd, className, 64);
                if (wcscmp(className, L"WmuxWindowClass") == 0)
                    newExternalTarget = targetHwnd;
            }
        }

        if (m_externalDropTarget && m_externalDropTarget != newExternalTarget)
            CancelDragPreview(m_externalDropTarget);
        m_externalDropTarget = newExternalTarget;

        if (insideWindow) {
            IPaneSession* targetPane = m_paneManager.FindPaneAtPoint(
                static_cast<float>(x), static_cast<float>(y));

            if (targetPane && targetPane != m_draggedPane) {
                m_dropTargetPane = targetPane;

                D2D1_RECT_F targetRect = {};
                m_paneManager.ForEachLeaf([&](SplitNode& node) {
                    if (node.pane.get() == targetPane)
                        targetRect = node.rect;
                });

                float relX = (x - targetRect.left) / (targetRect.right - targetRect.left);
                float relY = (y - targetRect.top) / (targetRect.bottom - targetRect.top);

                const float edgeSize = 0.25f;
                if (relY < edgeSize) m_dropZone = 0;
                else if (relY > 1.0f - edgeSize) m_dropZone = 2;
                else if (relX < edgeSize) m_dropZone = 3;
                else if (relX > 1.0f - edgeSize) m_dropZone = 1;
                else m_dropZone = 4;
            } else {
                m_dropTargetPane = nullptr;
                m_dropZone = -1;
            }
        } else {
            m_dropTargetPane = nullptr;
            m_dropZone = -1;

            if (m_externalDropTarget)
                SendDragPreview(m_externalDropTarget, screenPt.x, screenPt.y);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_draggingHelpScrollbar) {
        const int maxScroll = (std::max)(0, DxRenderer::GetHelpLineCount() - DxRenderer::HELP_VISIBLE_LINES);
        float deltaY = static_cast<float>(y) - m_helpDragStartY;
        int scrollDelta = static_cast<int>(deltaY / DxRenderer::HELP_DRAG_SENSITIVITY);

        m_helpScrollOffset = m_helpScrollOffsetAtDragStart + scrollDelta;
        if (m_helpScrollOffset < 0) m_helpScrollOffset = 0;
        if (m_helpScrollOffset > maxScroll) m_helpScrollOffset = maxScroll;

        InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_draggingSeparator) {
        ApplySeparatorDrag(x, y);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_draggingScrollbar) {
        ApplyScrollbarDrag(y);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_selecting) {
        int viewRow = 0;
        MouseToCell(x, y, m_selectPane, m_selectPaneRect, viewRow, m_selEndCol);
        m_selEndRow = m_selectPane->GetBuffer().ViewRowToDocumentRow(viewRow);
        UpdateSelectionAutoScroll();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void App::OnLButtonUp() {
    if (m_draggingPane) {
        if (m_externalDropTarget && m_draggedPane) {
            std::wstring sessionToken = m_draggedPane->GetSessionToken();
            if (!sessionToken.empty()) {
                POINT screenPt = {m_lastMouseX, m_lastMouseY};
                ClientToScreen(m_hwnd, &screenPt);

                bool wasSinglePane = m_paneManager.HasSinglePane();

                if (m_selectPane == m_draggedPane)
                    ClearSelection();
                if (m_dragPane == m_draggedPane)
                    m_dragPane = nullptr;
                if (m_lastDblClickPane == m_draggedPane)
                    m_lastDblClickPane = nullptr;


                m_draggedPane->PrepareForMove();
                m_draggedPane->Stop();

                std::wstring payload = std::to_wstring(screenPt.x) + L","
                    + std::to_wstring(screenPt.y) + L"\n" + sessionToken;
                COPYDATASTRUCT copyData = {};
                copyData.dwData = GetAttachPaneCopyDataId();
                copyData.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
                copyData.lpData = payload.data();
                DWORD_PTR sendResult = 0;
                LRESULT sendOk = SendMessageTimeoutW(m_externalDropTarget, WM_COPYDATA,
                                    reinterpret_cast<WPARAM>(m_hwnd),
                                    reinterpret_cast<LPARAM>(&copyData),
                                    SMTO_ABORTIFHUNG, 5000, &sendResult);

                bool accepted = (sendOk != 0) && (sendResult == TRUE);
                if (!accepted) {
                    auto recoveredPane = AttachPaneSession(sessionToken);
                    if (recoveredPane) {
                        RECT rc;
                        GetClientRect(m_hwnd, &rc);
                        float statusH = m_renderer.GetStatusBarHeight();
                        float paneH = static_cast<float>(rc.bottom) - statusH;
                        int cols = m_renderer.CalcCols(rc.right);
                        int rows = m_renderer.CalcRows(static_cast<UINT>(paneH));
                        uint32_t paneId = m_paneManager.AllocatePaneId();
                        if (recoveredPane->Start(cols, rows, m_hwnd, WM_PTY_OUTPUT, L"", paneId)) {
                            m_paneManager.SetActivePane(m_draggedPane);
                            SplitNode* activeNode = m_paneManager.GetActiveNode();
                            if (activeNode) {
                                activeNode->pane = std::move(recoveredPane);
                                activeNode->paneId = paneId;
                            }
                        }
                    }
                } else {
                    m_paneManager.RemoveActiveWithoutStopping();

                    if (wasSinglePane) {
                        m_draggingPane = false;
                        m_draggedPane = nullptr;
                        m_draggedNode = nullptr;
                        m_dropTargetPane = nullptr;
                        m_dropZone = -1;
                        m_externalDropTarget = nullptr;
                        ReleaseCapture();
                        DestroyWindow(m_hwnd);
                        return;
                    }
                    RelayoutAfterPaneRemoval();
                }
            }
        } else if (m_dropTargetPane && m_dropZone >= 0) {
            m_paneManager.InsertPaneAt(m_draggedPane, m_dropTargetPane, m_dropZone);

            RECT rc;
            GetClientRect(m_hwnd, &rc);
            float statusH = m_renderer.GetStatusBarHeight();
            float paneH = static_cast<float>(rc.bottom) - statusH;
            D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right), paneH};
            m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(), m_renderer.GetCellHeight());
        } else if (m_draggedPane) {
            RECT clientRect;
            GetClientRect(m_hwnd, &clientRect);
            bool insideWindow = (m_lastMouseX >= 0 && m_lastMouseY >= 0 &&
                                 m_lastMouseX < clientRect.right && m_lastMouseY < clientRect.bottom);
            if (!insideWindow) {
                DetachActivePaneToNewWindow();
            }
        }

        if (m_externalDropTarget)
            CancelDragPreview(m_externalDropTarget);
        m_draggingPane = false;
        m_draggedPane = nullptr;
        m_draggedNode = nullptr;
        m_dropTargetPane = nullptr;
        m_dropZone = -1;
        m_externalDropTarget = nullptr;
        ReleaseCapture();
        SetCursor(LoadCursor(nullptr, IDC_IBEAM));
        InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (m_draggingHelpScrollbar) {
        m_draggingHelpScrollbar = false;
        ReleaseCapture();
    } else if (m_draggingSeparator) {
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
        KillTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL);
        ReleaseCapture();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::OnRButtonUp(int x, int y) {
    // Block right-click when help is showing
    if (m_showHelp) {
        return;
    }

    IPaneSession* pane = nullptr;
    D2D1_RECT_F rect = {};
    if (m_paneManager.FindPaneAndRectAtPoint(static_cast<float>(x), static_cast<float>(y), pane, rect)) {
        m_paneManager.SetActivePane(pane);
        m_selectPaneRect = rect;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | (m_hasSelection ? MF_ENABLED : MF_GRAYED), 1, L"Copy");
    AppendMenuW(menu, MF_STRING, 2, L"Paste");
    AppendMenuW(menu, MF_STRING | (pane ? MF_ENABLED : MF_GRAYED), 3, L"Select All");

    POINT pt = {x, y};
    ClientToScreen(m_hwnd, &pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case 1:
        CopySelection();
        break;
    case 2:
        PasteClipboard();
        break;
    case 3:
        SelectAllVisible(pane ? pane : m_paneManager.GetActivePane());
        InvalidateRect(m_hwnd, nullptr, FALSE);
        break;
    }
}

void App::CancelMouseOperation() {
    KillTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL);

    m_draggingHelpScrollbar = false;
    m_draggingSeparator = false;
    m_dragSplitNode = nullptr;
    m_draggingScrollbar = false;
    m_dragPane = nullptr;
    m_selecting = false;

    if (GetCapture() == m_hwnd)
        ReleaseCapture();
}

void App::ClearSelection() {
    KillTimer(m_hwnd, TIMER_SELECTION_AUTOSCROLL);
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
            const Cell& cell = buf.CellAtDocumentRow(r, c);
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
    IPaneSession* active = m_paneManager.GetActivePane();
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
    Settings original = m_settings;
    if (ShowSettingsDialog(m_hwnd, m_settings, [this](const Settings& preview) {
            ApplyVisualSettings(preview);
        })) {
        ApplyVisualSettings(m_settings);
    } else {
        m_settings = original;
        ApplyVisualSettings(m_settings);
    }
}

void App::UpdateTitleBar() {
    if (m_inputMode == InputMode::Prefix)
        SetWindowTextW(m_hwnd, L"wmux [PREFIX]");
    else
        SetWindowTextW(m_hwnd, L"wmux");
}

void App::OnDropFolder(const std::wstring& path) {
    // Check if path is a directory
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return;

    // Split current pane vertically with new pane in dropped directory
    m_paneManager.SplitActive(SplitDirection::Vertical, m_hwnd, WM_PTY_OUTPUT,
                              m_renderer.GetCellWidth(), m_renderer.GetCellHeight(),
                              path);

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    float statusH = m_renderer.GetStatusBarHeight();
    D2D1_RECT_F paneRect = {0, 0, static_cast<float>(rc.right),
                             static_cast<float>(rc.bottom) - statusH};
    m_paneManager.Relayout(paneRect, m_renderer.GetCellWidth(),
                           m_renderer.GetCellHeight());
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void App::HandleResumeChoice(wchar_t choice) {
    if (choice == 'r' || choice == 'R') {
        IPaneSession* active = m_paneManager.GetActivePane();
        if (active) {
            for (size_t i = 0; i < m_pendingUserCommand.size(); i++)
                active->SendInput("\x7f", 1);
            char utf8[4096];
            int len = WideCharToMultiByte(CP_UTF8, 0, m_resumeCommand.c_str(),
                                          static_cast<int>(m_resumeCommand.size()),
                                          utf8, sizeof(utf8), nullptr, nullptr);
            if (len > 0)
                active->SendInput(utf8, len);
            active->SendInput("\r", 1);
            std::wstring cwd = active->GetWorkingDirectory();
            if (!cwd.empty())
                m_resumeManager.ClearResume(cwd, m_resumeAgentName);
        }
    } else if (choice == 'n' || choice == 'N') {
        IPaneSession* active = m_paneManager.GetActivePane();
        if (active) {
            active->GetBuffer().ScrollToBottom();
            active->SendInput("\r", 1);
            std::wstring cwd = active->GetWorkingDirectory();
            if (!cwd.empty())
                m_resumeManager.ClearResume(cwd, m_resumeAgentName);
        }
    } else if (choice == 0x1B) {
        // Cancel - don't send Enter
    } else {
        return;
    }

    m_showResumePrompt = false;
    m_resumeAgentName.clear();
    m_resumeCommand.clear();
    m_pendingUserCommand.clear();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}
