#include "settings_dialog.h"
#include <vector>
#include <string>
#include <cwctype>
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
static const int ID_SEPARATOR_COLOR_BTN = 107;
static const int ID_PREFIX_TIMEOUT = 108;
static const int ID_SCROLL_LINES = 109;
static const int ID_SHOW_PREFIX_OVERLAY = 110;
static const int ID_IDLE_SCRAMBLE = 111;
static const int ID_CLOSE_BTN = 112;
static const int ID_OK         = IDOK;
static const int ID_CANCEL     = IDCANCEL;

static constexpr int kHeaderHeight = 62;
static constexpr int kSidebarWidth = 210;
static constexpr int kFooterHeight = 76;
static constexpr COLORREF kWindowBg = RGB(244, 247, 250);
static constexpr COLORREF kContentBg = RGB(250, 252, 255);
static constexpr COLORREF kSidebarBg = RGB(21, 36, 46);
static constexpr COLORREF kSidebarAccent = RGB(70, 168, 150);
static constexpr COLORREF kAccent = RGB(44, 122, 242);
static constexpr COLORREF kAccentSoft = RGB(224, 236, 255);
static constexpr COLORREF kTextPrimary = RGB(29, 35, 43);
static constexpr COLORREF kTextMuted = RGB(100, 110, 124);
static constexpr COLORREF kBorder = RGB(213, 221, 232);
static constexpr COLORREF kInputBg = RGB(255, 255, 255);
static constexpr COLORREF kCardBg = RGB(255, 255, 255);

struct DialogData {
    Settings* settings;
    Settings originalSettings;
    SettingsPreviewCallback onPreview;
    HWND hFontCombo;
    HWND hFontSize;
    HWND hWinW;
    HWND hWinH;
    HWND hDimInactive;
    HWND hBgColorBtn;
    HWND hSeparatorColorBtn;
    HWND hPrefixTimeout;
    HWND hScrollLines;
    HWND hIdleScramble;
    HWND hShowPrefixOverlay;
    HWND hOk;
    HWND hCancel;
    HWND hClose;
    uint32_t bgColor;
    uint32_t separatorColor;
    HFONT hTitleFont = nullptr;
    HFONT hBodyFont = nullptr;
    HFONT hSmallFont = nullptr;
    int result = 0;  // 0 = Cancel, 1 = OK
    bool closing = false;  // Prevent double-processing of OK/Cancel
};

static HBRUSH GetSolidBrush(COLORREF color) {
    static COLORREF cachedColors[8] = {};
    static HBRUSH cachedBrushes[8] = {};
    for (int i = 0; i < 8; ++i) {
        if (cachedBrushes[i] && cachedColors[i] == color)
            return cachedBrushes[i];
    }
    for (int i = 0; i < 8; ++i) {
        if (!cachedBrushes[i]) {
            cachedColors[i] = color;
            cachedBrushes[i] = CreateSolidBrush(color);
            return cachedBrushes[i];
        }
    }
    return CreateSolidBrush(color);
}

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

static HWND CreateSectionLabel(HWND parent, const wchar_t* text, int x, int y, int w, HFONT font) {
    HWND hw = CreateLabel(parent, text, x, y, w, 22);
    SendMessage(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static HWND CreateEdit(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND hw = CreateWindowExW(0, L"EDIT", text,
                              WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandle(nullptr), nullptr);
    return hw;
}

static bool ReadFloatInRange(HWND edit, const wchar_t* fieldName, float minValue,
                             float maxValue, float& outValue) {
    wchar_t buf[256] = {};
    GetWindowTextW(edit, buf, 256);
    wchar_t* end = nullptr;
    double value = wcstod(buf, &end);
    while (end && *end && iswspace(*end)) ++end;
    if (buf[0] == L'\0' || end == buf || (end && *end != L'\0') ||
        value < minValue || value > maxValue) {
        wchar_t msg[256];
        swprintf_s(msg, L"%s must be between %.1f and %.1f.", fieldName, minValue, maxValue);
        MessageBoxW(edit, msg, L"Invalid Setting", MB_OK | MB_ICONWARNING);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        return false;
    }
    outValue = static_cast<float>(value);
    return true;
}

static bool ReadIntMin(HWND edit, const wchar_t* fieldName, int minValue, int& outValue) {
    wchar_t buf[256] = {};
    GetWindowTextW(edit, buf, 256);
    wchar_t* end = nullptr;
    long value = wcstol(buf, &end, 10);
    while (end && *end && iswspace(*end)) ++end;
    if (buf[0] == L'\0' || end == buf || (end && *end != L'\0') || value < minValue) {
        wchar_t msg[256];
        swprintf_s(msg, L"%s must be %d or greater.", fieldName, minValue);
        MessageBoxW(edit, msg, L"Invalid Setting", MB_OK | MB_ICONWARNING);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        return false;
    }
    outValue = static_cast<int>(value);
    return true;
}

static bool TryReadFloat(HWND edit, float minValue, float maxValue, float& outValue) {
    wchar_t buf[256] = {};
    GetWindowTextW(edit, buf, 256);
    wchar_t* end = nullptr;
    double value = wcstod(buf, &end);
    while (end && *end && iswspace(*end)) ++end;
    if (buf[0] == L'\0' || end == buf || (end && *end != L'\0') ||
        value < minValue || value > maxValue) {
        return false;
    }
    outValue = static_cast<float>(value);
    return true;
}

static bool TryReadInt(HWND edit, int minValue, int maxValue, int& outValue) {
    wchar_t buf[256] = {};
    GetWindowTextW(edit, buf, 256);
    wchar_t* end = nullptr;
    long value = wcstol(buf, &end, 10);
    while (end && *end && iswspace(*end)) ++end;
    if (buf[0] == L'\0' || end == buf || (end && *end != L'\0') ||
        value < minValue || value > maxValue) {
        return false;
    }
    outValue = static_cast<int>(value);
    return true;
}

static void NotifyPreview(DialogData* data) {
    if (!data || !data->onPreview) return;

    Settings preview = *data->settings;
    preview.backgroundColor = data->bgColor;
    preview.separatorColor = data->separatorColor;
    preview.dimInactivePanes =
        (SendMessage(data->hDimInactive, BM_GETCHECK, 0, 0) == BST_CHECKED);
    preview.showPrefixOverlay =
        (SendMessage(data->hShowPrefixOverlay, BM_GETCHECK, 0, 0) == BST_CHECKED);

    wchar_t buf[256] = {};
    int idx = (int)SendMessageW(data->hFontCombo, CB_GETCURSEL, 0, 0);
    if (idx >= 0) {
        SendMessageW(data->hFontCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
        preview.fontName = buf;
    }

    float size = 0.0f;
    if (TryReadFloat(data->hFontSize, 6.0f, 72.0f, size))
        preview.fontSize = size;

    int timeout = 0;
    if (TryReadInt(data->hPrefixTimeout, 250, 10000, timeout))
        preview.prefixTimeoutMs = timeout;

    int scrollLines = 0;
    if (TryReadInt(data->hScrollLines, 0, 100, scrollLines))
        preview.scrollLines = scrollLines;

    int idleScramble = 0;
    if (TryReadInt(data->hIdleScramble, 0, 240, idleScramble))
        preview.idleScrambleMinutes = idleScramble;

    data->onPreview(preview);
}

static LRESULT CALLBACK DlgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogData* data = reinterpret_cast<DialogData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        data = static_cast<DialogData*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        RECT rc;
        GetClientRect(hwnd, &rc);
        HRGN hRgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, 18, 18);
        SetWindowRgn(hwnd, hRgn, TRUE);

        data->hTitleFont = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        data->hBodyFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        data->hSmallFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        const int contentX = kSidebarWidth + 24;
        const int topY = 30;
        const int cardGap = 12;
        const int cardW = 240;
        const int leftCardX = contentX;
        const int rightCardX = contentX + cardW + cardGap;
        const int cardTop = topY + 10;
        const int cardInnerLeft = 16;
        const int cardInnerTop = 16;
        const int labelW = 110;
        const int controlW = 92;
        const int comboW = 126;
        const int rowGap = 34;

        HWND hLabel = CreateSectionLabel(hwnd, L"\uAE00\uAF34", leftCardX + cardInnerLeft, cardTop + cardInnerTop, 140, data->hBodyFont);

        int y = cardTop + 50;
        hLabel = CreateLabel(hwnd, L"\uAE00\uAF34", leftCardX + cardInnerLeft, y + 4, labelW, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        data->hFontCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER | CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL | WS_TABSTOP,
            leftCardX + 98, y, comboW, 260, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FONT_COMBO)),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(data->hFontCombo, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        auto fonts = EnumMonoFonts();
        for (auto& f : fonts)
            SendMessageW(data->hFontCombo, CB_ADDSTRING, 0, (LPARAM)f.c_str());
        int selIdx = (int)SendMessageW(data->hFontCombo, CB_FINDSTRINGEXACT,
                                        (WPARAM)-1, (LPARAM)data->settings->fontName.c_str());
        if (selIdx == CB_ERR) selIdx = 0;
        SendMessageW(data->hFontCombo, CB_SETCURSEL, selIdx, 0);

        y += rowGap;
        hLabel = CreateLabel(hwnd, L"\uD06C\uAE30", leftCardX + cardInnerLeft, y + 4, labelW, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f", data->settings->fontSize);
        data->hFontSize = CreateEdit(hwnd, buf, ID_FONT_SIZE, leftCardX + 98, y, controlW, 24);
        SendMessage(data->hFontSize, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        const int rightTop = cardTop;
        hLabel = CreateSectionLabel(hwnd, L"\uCC3D", rightCardX + cardInnerLeft, rightTop + cardInnerTop, 140, data->hBodyFont);

        y = rightTop + 50;
        hLabel = CreateLabel(hwnd, L"\uB108\uBE44", rightCardX + cardInnerLeft, y + 4, labelW, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        swprintf_s(buf, L"%d", data->settings->windowWidth);
        data->hWinW = CreateEdit(hwnd, buf, ID_WIN_W, rightCardX + 98, y, controlW, 24);
        SendMessage(data->hWinW, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        y += rowGap;
        hLabel = CreateLabel(hwnd, L"\uB192\uC774", rightCardX + cardInnerLeft, y + 4, labelW, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        swprintf_s(buf, L"%d", data->settings->windowHeight);
        data->hWinH = CreateEdit(hwnd, buf, ID_WIN_H, rightCardX + 98, y, controlW, 24);
        SendMessage(data->hWinH, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        const int secondRowTop = 214;
        hLabel = CreateSectionLabel(hwnd, L"\uD654\uBA74", leftCardX + cardInnerLeft, secondRowTop + cardInnerTop, 140, data->hBodyFont);

        y = secondRowTop + 50;
        data->hDimInactive = CreateWindowExW(0, L"BUTTON", L"\uBE44\uD65C\uC131 pane \uC5B4\uB465\uAC8C",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            leftCardX + cardInnerLeft, y, 170, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DIM_INACTIVE)),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(data->hDimInactive, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        SendMessage(data->hDimInactive, BM_SETCHECK,
                    data->settings->dimInactivePanes ? BST_CHECKED : BST_UNCHECKED, 0);

        y += rowGap;
        hLabel = CreateLabel(hwnd, L"\uBC30\uACBD\uC0C9", leftCardX + cardInnerLeft, y + 4, labelW, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        data->hBgColorBtn = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            leftCardX + 98, y - 2, 96, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BG_COLOR_BTN)),
            GetModuleHandle(nullptr), nullptr);
        data->bgColor = data->settings->backgroundColor;
        SendMessage(data->hBgColorBtn, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        y += 50;
        hLabel = CreateLabel(hwnd, L"\uBD84\uD560\uC120 \uC0C9", leftCardX + cardInnerLeft, y + 4, labelW, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        data->hSeparatorColorBtn = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            leftCardX + 98, y - 2, 96, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEPARATOR_COLOR_BTN)),
            GetModuleHandle(nullptr), nullptr);
        data->separatorColor = data->settings->separatorColor;
        SendMessage(data->hSeparatorColorBtn, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        y += 50;
        data->hShowPrefixOverlay = CreateWindowExW(0, L"BUTTON", L"\uD504\uB9AC\uD53D\uC2A4 \uC624\uBC84\uB808\uC774 \uD45C\uC2DC",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            leftCardX + cardInnerLeft, y, 180, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SHOW_PREFIX_OVERLAY)),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(data->hShowPrefixOverlay, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        SendMessage(data->hShowPrefixOverlay, BM_SETCHECK,
                    data->settings->showPrefixOverlay ? BST_CHECKED : BST_UNCHECKED, 0);

        hLabel = CreateSectionLabel(hwnd, L"\uB3D9\uC791", rightCardX + cardInnerLeft, secondRowTop + cardInnerTop, 140, data->hBodyFont);

        y = secondRowTop + 50;
        hLabel = CreateLabel(hwnd, L"\uD504\uB9AC\uD53D\uC2A4 \uC2DC\uAC04", rightCardX + cardInnerLeft, y + 4, 110, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        swprintf_s(buf, L"%d", data->settings->prefixTimeoutMs);
        data->hPrefixTimeout = CreateEdit(hwnd, buf, ID_PREFIX_TIMEOUT, rightCardX + 108, y, controlW, 24);
        SendMessage(data->hPrefixTimeout, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        y += 50;
        hLabel = CreateLabel(hwnd, L"\uC2A4\uD06C\uB864 \uC904 \uC218", rightCardX + cardInnerLeft, y + 4, 110, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        swprintf_s(buf, L"%d", data->settings->scrollLines);
        data->hScrollLines = CreateEdit(hwnd, buf, ID_SCROLL_LINES, rightCardX + 108, y, controlW, 24);
        SendMessage(data->hScrollLines, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        {
            HWND hHint = CreateLabel(hwnd, L"0 = Windows \uC124\uC815, \uD720 1\uCE78\uB2F9 \uC904 \uC218",
                                     rightCardX + cardInnerLeft, y + 28, 220, 18);
            SendMessage(hHint, WM_SETFONT, (WPARAM)data->hSmallFont, TRUE);
        }

        y += 50;
        hLabel = CreateLabel(hwnd, L"\uC720\uD734 \uD6A8\uACFC(\uBD84)", rightCardX + cardInnerLeft, y + 4, 110, 20);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        swprintf_s(buf, L"%d", data->settings->idleScrambleMinutes);
        data->hIdleScramble = CreateEdit(hwnd, buf, ID_IDLE_SCRAMBLE, rightCardX + 108, y, controlW, 24);
        SendMessage(data->hIdleScramble, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        {
            HWND hHint = CreateLabel(hwnd, L"0 = \uAE30\uB2A5 \uB044\uAE30, \uC785\uB825 \uC5C6\uC74C \uC2DC \uD654\uBA74 \uAE00\uC790 \uBCC0\uD615",
                                     rightCardX + cardInnerLeft, y + 28, 250, 18);
            SendMessage(hHint, WM_SETFONT, (WPARAM)data->hSmallFont, TRUE);
        }

        data->hClose = CreateWindowExW(0, L"BUTTON", L"X",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            clientRc.right - 42, 16, 26, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLOSE_BTN)),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(data->hClose, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        data->hOk = CreateWindowExW(0, L"BUTTON", L"\uC800\uC7A5",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_DEFPUSHBUTTON | WS_TABSTOP,
            clientRc.right - 246, clientRc.bottom - 52, 128, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OK)),
            GetModuleHandle(nullptr), nullptr);
        data->hCancel = CreateWindowExW(0, L"BUTTON", L"\uCDE8\uC18C",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            clientRc.right - 106, clientRc.bottom - 52, 84, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CANCEL)),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(data->hOk, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);
        SendMessage(data->hCancel, WM_SETFONT, (WPARAM)data->hBodyFont, TRUE);

        return 0;
    }

    case WM_DRAWITEM: {
        if (!data) break;
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        RECT rc = dis->rcItem;
        HDC hdc = dis->hDC;
        SetBkMode(hdc, TRANSPARENT);

        if (dis->CtlID == ID_BG_COLOR_BTN || dis->CtlID == ID_SEPARATOR_COLOR_BTN) {
            uint32_t color = (dis->CtlID == ID_BG_COLOR_BTN) ? data->bgColor : data->separatorColor;
            HBRUSH hBrush = CreateSolidBrush(RGB(
                (color >> 16) & 0xFF,
                (color >> 8) & 0xFF,
                color & 0xFF));
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
            FrameRect(hdc, &rc, GetSolidBrush(kBorder));
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, L"\uC120\uD0DD", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        if (dis->CtlID == ID_OK || dis->CtlID == ID_CANCEL || dis->CtlID == ID_CLOSE_BTN) {
            COLORREF fill = kCardBg;
            COLORREF text = kTextPrimary;
            COLORREF border = kBorder;
            if (dis->CtlID == ID_OK) {
                fill = kAccent;
                text = RGB(255, 255, 255);
                border = kAccent;
            } else if (dis->CtlID == ID_CLOSE_BTN) {
                fill = RGB(34, 57, 73);
                text = RGB(255, 255, 255);
                border = RGB(82, 97, 112);
            }
            if (dis->itemState & ODS_SELECTED) {
                fill = RGB((GetRValue(fill) * 9) / 10, (GetGValue(fill) * 9) / 10, (GetBValue(fill) * 9) / 10);
            }
            FillRect(hdc, &rc, GetSolidBrush(fill));
            FrameRect(hdc, &rc, GetSolidBrush(border));
            SetTextColor(hdc, text);
            wchar_t textBuf[32] = {};
            GetWindowTextW(dis->hwndItem, textBuf, 32);
            DrawTextW(hdc, textBuf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kTextPrimary);
        return reinterpret_cast<INT_PTR>(GetStockObject(HOLLOW_BRUSH));
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, kInputBg);
        SetTextColor(hdc, kTextPrimary);
        return reinterpret_cast<INT_PTR>(GetSolidBrush(kInputBg));
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kTextPrimary);
        return reinterpret_cast<INT_PTR>(GetSolidBrush(kCardBg));
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HRGN hRgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, 18, 18);
        SetWindowRgn(hwnd, hRgn, TRUE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, GetSolidBrush(kWindowBg));

        RECT sidebar = {0, 0, kSidebarWidth, rc.bottom};
        FillRect(hdc, &sidebar, GetSolidBrush(kSidebarBg));

        RECT content = {kSidebarWidth, 0, rc.right, rc.bottom};
        FillRect(hdc, &content, GetSolidBrush(kContentBg));

        RECT accentBar = {24, 26, 30, 76};
        FillRect(hdc, &accentBar, GetSolidBrush(kSidebarAccent));

        SetBkMode(hdc, TRANSPARENT);
        HFONT oldFont = (HFONT)SelectObject(hdc, data && data->hTitleFont ? data->hTitleFont : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(hdc, RGB(255, 255, 255));
        RECT titleRect = {44, 18, kSidebarWidth - 18, 54};
        DrawTextW(hdc, L"\uC124\uC815", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        auto drawCard = [&](RECT card) {
            FillRect(hdc, &card, GetSolidBrush(kCardBg));
            FrameRect(hdc, &card, GetSolidBrush(kBorder));
        };

        RECT card1 = {kSidebarWidth + 14, 42, kSidebarWidth + 254, 182};
        RECT card2 = {kSidebarWidth + 266, 42, kSidebarWidth + 506, 182};
        RECT card3 = {kSidebarWidth + 14, 206, kSidebarWidth + 254, 476};
        RECT card4 = {kSidebarWidth + 266, 206, kSidebarWidth + 506, 438};
        drawCard(card1);
        drawCard(card2);
        drawCard(card3);
        drawCard(card4);

        HPEN dividerPen = CreatePen(PS_SOLID, 1, RGB(232, 237, 244));
        HPEN oldPen = (HPEN)SelectObject(hdc, dividerPen);
        MoveToEx(hdc, card1.left + 16, card1.top + 48, nullptr); LineTo(hdc, card1.right - 16, card1.top + 48);
        MoveToEx(hdc, card2.left + 16, card2.top + 48, nullptr); LineTo(hdc, card2.right - 16, card2.top + 48);
        MoveToEx(hdc, card3.left + 16, card3.top + 48, nullptr); LineTo(hdc, card3.right - 16, card3.top + 48);
        MoveToEx(hdc, card4.left + 16, card4.top + 48, nullptr); LineTo(hdc, card4.right - 16, card4.top + 48);
        SelectObject(hdc, oldPen);
        DeleteObject(dividerPen);

        RECT footer = {kSidebarWidth, rc.bottom - kFooterHeight, rc.right, rc.bottom};
        FillRect(hdc, &footer, GetSolidBrush(RGB(243, 247, 252)));
        HPEN footerPen = CreatePen(PS_SOLID, 1, kBorder);
        oldPen = (HPEN)SelectObject(hdc, footerPen);
        MoveToEx(hdc, footer.left, footer.top, nullptr);
        LineTo(hdc, footer.right, footer.top);
        SelectObject(hdc, oldPen);
        DeleteObject(footerPen);

        SelectObject(hdc, oldFont);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProc(hwnd, msg, wParam, lParam);
        if (hit != HTCLIENT)
            return hit;
        POINT pt = {static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                    static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < kHeaderHeight) {
            RECT rcClose = {};
            if (data && data->hClose)
                GetWindowRect(data->hClose, &rcClose);
            POINT screenPt = {static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                              static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
            if (!PtInRect(&rcClose, screenPt))
                return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_COMMAND:
        if (!data) break;
        if ((LOWORD(wParam) == ID_FONT_COMBO && HIWORD(wParam) == CBN_SELCHANGE) ||
            ((LOWORD(wParam) == ID_FONT_SIZE || LOWORD(wParam) == ID_DIM_INACTIVE ||
              LOWORD(wParam) == ID_SHOW_PREFIX_OVERLAY || LOWORD(wParam) == ID_PREFIX_TIMEOUT ||
              LOWORD(wParam) == ID_SCROLL_LINES || LOWORD(wParam) == ID_IDLE_SCRAMBLE) &&
             (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == BN_CLICKED))) {
            NotifyPreview(data);
            return 0;
        }
        // Only handle button clicks (BN_CLICKED), ignore other notifications
        if (HIWORD(wParam) != BN_CLICKED && HIWORD(wParam) != 0) break;
        switch (LOWORD(wParam)) {
        case ID_BG_COLOR_BTN:
        case ID_SEPARATOR_COLOR_BTN: {
            if (HIWORD(wParam) != BN_CLICKED) break;

            // Show color picker dialog
            CHOOSECOLOR cc = {};
            static COLORREF customColors[16] = {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = customColors;
            uint32_t currentColor = (LOWORD(wParam) == ID_BG_COLOR_BTN)
                ? data->bgColor : data->separatorColor;
            cc.rgbResult = RGB(
                (currentColor >> 16) & 0xFF,
                (currentColor >> 8) & 0xFF,
                currentColor & 0xFF);
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            if (ChooseColor(&cc)) {
                uint32_t chosenColor = ((GetRValue(cc.rgbResult) << 16) |
                                        (GetGValue(cc.rgbResult) << 8) |
                                        GetBValue(cc.rgbResult));
                HWND targetButton = data->hBgColorBtn;
                if (LOWORD(wParam) == ID_BG_COLOR_BTN) {
                    data->bgColor = chosenColor;
                } else {
                    data->separatorColor = chosenColor;
                    targetButton = data->hSeparatorColorBtn;
                }
                InvalidateRect(targetButton, nullptr, TRUE);
                NotifyPreview(data);
            }
            return 0;
        }
        case ID_OK: {
            if (HIWORD(wParam) != BN_CLICKED && HIWORD(wParam) != 0) break;
            if (data->closing) return 0;  // Already processing close

            // Read values
            wchar_t buf[256];

            int idx = (int)SendMessageW(data->hFontCombo, CB_GETCURSEL, 0, 0);
            if (idx < 0) {
                MessageBoxW(hwnd, L"Select a font before saving.", L"Invalid Setting",
                            MB_OK | MB_ICONWARNING);
                SetFocus(data->hFontCombo);
                return 0;
            }
            SendMessageW(data->hFontCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
            data->settings->fontName = buf;

            float sz = 0.0f;
            if (!ReadFloatInRange(data->hFontSize, L"Font size", 6.0f, 72.0f, sz))
                return 0;
            data->settings->fontSize = sz;

            int w = 0;
            if (!ReadIntMin(data->hWinW, L"Window width", 200, w))
                return 0;
            data->settings->windowWidth = w;

            int h = 0;
            if (!ReadIntMin(data->hWinH, L"Window height", 150, h))
                return 0;
            data->settings->windowHeight = h;

            int timeout = 0;
            if (!ReadIntMin(data->hPrefixTimeout, L"Prefix timeout", 250, timeout))
                return 0;
            if (timeout > 10000) {
                MessageBoxW(hwnd, L"Prefix timeout must be 10000 or less.", L"Invalid Setting",
                            MB_OK | MB_ICONWARNING);
                SetFocus(data->hPrefixTimeout);
                SendMessageW(data->hPrefixTimeout, EM_SETSEL, 0, -1);
                return 0;
            }
            data->settings->prefixTimeoutMs = timeout;

            int scrollLines = 0;
            if (!ReadIntMin(data->hScrollLines, L"Scroll lines", 0, scrollLines))
                return 0;
            if (scrollLines > 100) {
                MessageBoxW(hwnd, L"Scroll lines must be 100 or less.", L"Invalid Setting",
                            MB_OK | MB_ICONWARNING);
                SetFocus(data->hScrollLines);
                SendMessageW(data->hScrollLines, EM_SETSEL, 0, -1);
                return 0;
            }
            data->settings->scrollLines = scrollLines;

            int idleScramble = 0;
            if (!ReadIntMin(data->hIdleScramble, L"Idle effect minutes", 0, idleScramble))
                return 0;
            if (idleScramble > 240) {
                MessageBoxW(hwnd, L"Idle effect minutes must be 240 or less.", L"Invalid Setting",
                            MB_OK | MB_ICONWARNING);
                SetFocus(data->hIdleScramble);
                SendMessageW(data->hIdleScramble, EM_SETSEL, 0, -1);
                return 0;
            }
            data->settings->idleScrambleMinutes = idleScramble;

            data->settings->dimInactivePanes =
                (SendMessage(data->hDimInactive, BM_GETCHECK, 0, 0) == BST_CHECKED);
            data->settings->showPrefixOverlay =
                (SendMessage(data->hShowPrefixOverlay, BM_GETCHECK, 0, 0) == BST_CHECKED);

            data->settings->backgroundColor = data->bgColor;
            data->settings->separatorColor = data->separatorColor;

            data->closing = true;
            data->settings->Save();
            data->result = 1;  // OK
            DestroyWindow(hwnd);
            return 0;
        }
        case ID_CANCEL:
        case ID_CLOSE_BTN:
            if (HIWORD(wParam) != BN_CLICKED && HIWORD(wParam) != 0) break;
            if (data->closing) return 0;  // Already processing close
            data->closing = true;
            data->result = 0;  // Cancel
            if (data->onPreview)
                data->onPreview(data->originalSettings);
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;  // Message handled

    case WM_CLOSE:
        if (data) {
            data->result = 0;  // Treat close button as Cancel
            if (data->onPreview)
                data->onPreview(data->originalSettings);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (data) {
            if (data->hTitleFont) DeleteObject(data->hTitleFont);
            if (data->hBodyFont) DeleteObject(data->hBodyFont);
            if (data->hSmallFont) DeleteObject(data->hSmallFont);
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool ShowSettingsDialog(HWND parent, Settings& settings,
                        const SettingsPreviewCallback& onPreview) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DlgWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"WmuxSettingsClass";
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    Settings copy = settings;
    DialogData data = {};
    data.settings = &copy;
    data.originalSettings = settings;
    data.onPreview = onPreview;

    int dlgW = 760, dlgH = 560;
    RECT parentRect;
    GetWindowRect(parent, &parentRect);
    int x = parentRect.left + (parentRect.right - parentRect.left - dlgW) / 2;
    int y = parentRect.top + (parentRect.bottom - parentRect.top - dlgH) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"WmuxSettingsClass", L"wmux Settings",
        WS_POPUP | WS_SYSMENU,
        x, y, dlgW, dlgH,
        parent, nullptr, GetModuleHandle(nullptr), &data);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg = {};
    while (IsWindow(dlg)) {
        BOOL ret = GetMessage(&msg, nullptr, 0, 0);
        if (ret <= 0) break;
        if (!IsDialogMessage(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    SetFocus(parent);

    if (data.result == 1) {
        settings = copy;
        return true;
    }
    return false;
}
