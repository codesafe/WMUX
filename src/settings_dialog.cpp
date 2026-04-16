#include "settings_dialog.h"
#include <vector>
#include <string>
#include <CommCtrl.h>
#include <commdlg.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

static const int ID_FONT_COMBO = 101;
static const int ID_FONT_SIZE  = 102;
static const int ID_WIN_W      = 103;
static const int ID_WIN_H      = 104;
static const int ID_DIM_INACTIVE = 105;
static const int ID_BG_COLOR_BTN = 106;
static const int ID_OK         = IDOK;
static const int ID_CANCEL     = IDCANCEL;

struct DialogData {
    Settings* settings;
    HWND hFontCombo;
    HWND hFontSize;
    HWND hWinW;
    HWND hWinH;
    HWND hDimInactive;
    HWND hBgColorBtn;
    uint32_t bgColor;
    int result = 0;  // 0 = Cancel, 1 = OK
    bool closing = false;  // Prevent double-processing of OK/Cancel
};

static int CALLBACK EnumFontProc(const LOGFONTW* lf, const TEXTMETRICW*,
                                  DWORD fontType, LPARAM lParam) {
    (void)fontType;
    auto* fonts = reinterpret_cast<std::vector<std::wstring>*>(lParam);

    // Fixed-pitch fonts only
    if (lf->lfPitchAndFamily & FIXED_PITCH) {
        std::wstring name = lf->lfFaceName;
        // Avoid duplicates and @-prefixed vertical fonts
        if (!name.empty() && name[0] != L'@') {
            for (auto& f : *fonts) {
                if (f == name) return 1;
            }
            fonts->push_back(name);
        }
    }
    return 1;
}

static std::vector<std::wstring> EnumMonoFonts() {
    std::vector<std::wstring> fonts;
    HDC hdc = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH;
    EnumFontFamiliesExW(hdc, &lf, (FONTENUMPROCW)EnumFontProc, (LPARAM)&fonts, 0);
    ReleaseDC(nullptr, hdc);
    return fonts;
}

static HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           x, y, w, h, parent, nullptr,
                           GetModuleHandle(nullptr), nullptr);
}

static HWND CreateEdit(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandle(nullptr), nullptr);
    return hw;
}

static LRESULT CALLBACK DlgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogData* data = reinterpret_cast<DialogData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        data = static_cast<DialogData*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));

        HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        int y = 15;
        int lx = 15, ex = 130, ew = 220;

        // Font name
        CreateLabel(hwnd, L"Font:", lx, y + 3, 100, 20);
        data->hFontCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL,
            ex, y, ew, 300, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FONT_COMBO)),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(data->hFontCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

        auto fonts = EnumMonoFonts();
        for (auto& f : fonts)
            SendMessageW(data->hFontCombo, CB_ADDSTRING, 0, (LPARAM)f.c_str());
        int selIdx = (int)SendMessageW(data->hFontCombo, CB_FINDSTRINGEXACT,
                                        (WPARAM)-1, (LPARAM)data->settings->fontName.c_str());
        if (selIdx == CB_ERR) selIdx = 0;
        SendMessageW(data->hFontCombo, CB_SETCURSEL, selIdx, 0);

        // Font size
        y += 35;
        CreateLabel(hwnd, L"Size:", lx, y + 3, 100, 20);
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f", data->settings->fontSize);
        data->hFontSize = CreateEdit(hwnd, buf, ID_FONT_SIZE, ex, y, 80, 24);
        SendMessage(data->hFontSize, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Window width
        y += 40;
        CreateLabel(hwnd, L"Window Width:", lx, y + 3, 110, 20);
        swprintf_s(buf, L"%d", data->settings->windowWidth);
        data->hWinW = CreateEdit(hwnd, buf, ID_WIN_W, ex, y, 80, 24);
        SendMessage(data->hWinW, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Window height
        y += 35;
        CreateLabel(hwnd, L"Window Height:", lx, y + 3, 110, 20);
        swprintf_s(buf, L"%d", data->settings->windowHeight);
        data->hWinH = CreateEdit(hwnd, buf, ID_WIN_H, ex, y, 80, 24);
        SendMessage(data->hWinH, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Dim inactive panes checkbox
        y += 40;
        data->hDimInactive = CreateWindowExW(0, L"BUTTON", L"Dim inactive panes",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            lx, y, 200, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DIM_INACTIVE)),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(data->hDimInactive, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(data->hDimInactive, BM_SETCHECK,
                    data->settings->dimInactivePanes ? BST_CHECKED : BST_UNCHECKED, 0);

        // Background color
        y += 35;
        CreateLabel(hwnd, L"Background Color:", lx, y + 3, 110, 20);
        data->hBgColorBtn = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            ex, y, 60, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BG_COLOR_BTN)),
            GetModuleHandle(nullptr), nullptr);
        data->bgColor = data->settings->backgroundColor;

        // Buttons
        y += 45;
        HWND hOk = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            130, y, 80, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OK)),
            GetModuleHandle(nullptr), nullptr);
        HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            220, y, 80, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CANCEL)),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Set font for labels
        EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
            wchar_t cls[32];
            GetClassNameW(child, cls, 32);
            if (wcscmp(cls, L"Static") == 0)
                SendMessage(child, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        }, (LPARAM)hFont);

        return 0;
    }

    case WM_DRAWITEM: {
        if (!data) break;
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (dis->CtlID == ID_BG_COLOR_BTN) {
            // Draw color button
            HBRUSH hBrush = CreateSolidBrush(RGB(
                (data->bgColor >> 16) & 0xFF,
                (data->bgColor >> 8) & 0xFF,
                data->bgColor & 0xFF));
            FillRect(dis->hDC, &dis->rcItem, hBrush);
            DeleteObject(hBrush);

            // Draw border
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
            HPEN hOldPen = (HPEN)SelectObject(dis->hDC, hPen);
            HBRUSH hOldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
                      dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, hOldPen);
            SelectObject(dis->hDC, hOldBrush);
            DeleteObject(hPen);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (!data) break;
        // Only handle button clicks (BN_CLICKED), ignore other notifications
        if (HIWORD(wParam) != BN_CLICKED && HIWORD(wParam) != 0) break;
        switch (LOWORD(wParam)) {
        case ID_BG_COLOR_BTN: {
            if (HIWORD(wParam) != BN_CLICKED) break;

            // Show color picker dialog
            CHOOSECOLOR cc = {};
            static COLORREF customColors[16] = {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = customColors;
            cc.rgbResult = RGB(
                (data->bgColor >> 16) & 0xFF,
                (data->bgColor >> 8) & 0xFF,
                data->bgColor & 0xFF);
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            if (ChooseColor(&cc)) {
                data->bgColor = ((GetRValue(cc.rgbResult) << 16) |
                                 (GetGValue(cc.rgbResult) << 8) |
                                 GetBValue(cc.rgbResult));
                InvalidateRect(data->hBgColorBtn, nullptr, TRUE);
            }
            return 0;
        }
        case ID_OK: {
            if (HIWORD(wParam) != BN_CLICKED && HIWORD(wParam) != 0) break;
            if (data->closing) return 0;  // Already processing close
            data->closing = true;

            // Read values
            wchar_t buf[256];

            int idx = (int)SendMessageW(data->hFontCombo, CB_GETCURSEL, 0, 0);
            if (idx >= 0)
                SendMessageW(data->hFontCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
            data->settings->fontName = buf;

            GetWindowTextW(data->hFontSize, buf, 256);
            float sz = static_cast<float>(_wtof(buf));
            if (sz >= 6.0f && sz <= 72.0f)
                data->settings->fontSize = sz;

            GetWindowTextW(data->hWinW, buf, 256);
            int w = _wtoi(buf);
            if (w >= 200) data->settings->windowWidth = w;

            GetWindowTextW(data->hWinH, buf, 256);
            int h = _wtoi(buf);
            if (h >= 150) data->settings->windowHeight = h;

            data->settings->dimInactivePanes =
                (SendMessage(data->hDimInactive, BM_GETCHECK, 0, 0) == BST_CHECKED);

            data->settings->backgroundColor = data->bgColor;

            data->settings->Save();
            data->result = 1;  // OK
            DestroyWindow(hwnd);
            return 0;
        }
        case ID_CANCEL:
            if (HIWORD(wParam) != BN_CLICKED && HIWORD(wParam) != 0) break;
            if (data->closing) return 0;  // Already processing close
            data->closing = true;
            data->result = 0;  // Cancel
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;  // Message handled

    case WM_CLOSE:
        if (data) data->result = 0;  // Treat close button as Cancel
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (data) PostQuitMessage(data->result);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool ShowSettingsDialog(HWND parent, Settings& settings) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DlgWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"WmuxSettingsClass";
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    Settings copy = settings;
    DialogData data = {&copy};

    int dlgW = 380, dlgH = 335;
    RECT parentRect;
    GetWindowRect(parent, &parentRect);
    int x = parentRect.left + (parentRect.right - parentRect.left - dlgW) / 2;
    int y = parentRect.top + (parentRect.bottom - parentRect.top - dlgH) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"WmuxSettingsClass", L"wmux Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, dlgW, dlgH,
        parent, nullptr, GetModuleHandle(nullptr), &data);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg = {};
    while (true) {
        BOOL ret = GetMessage(&msg, nullptr, 0, 0);
        if (ret == 0 || ret == -1) break;  // WM_QUIT or error
        if (!IsDialogMessage(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    SetFocus(parent);

    // GetMessage returns 0 on WM_QUIT; wParam carries our result code
    if (msg.message == WM_QUIT && msg.wParam == 1) {
        settings = copy;
        return true;
    }
    return false;
}
