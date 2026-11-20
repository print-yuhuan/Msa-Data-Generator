#pragma once
#include <windows.h>
#include <commctrl.h>
#include <cstdlib>
#include <cwchar>
#include <initializer_list>
#include "msa/brand.hpp"
#include "worksheet_ids.h"

namespace platform::replicate_dialog {
constexpr COLORREF background = RGB(242, 245, 249), surface = RGB(255, 255, 255),
                   border = RGB(220, 228, 238), ink = RGB(24, 44, 69),
                   muted = RGB(82, 100, 122), hover = RGB(237, 243, 250),
                   selectedFill = RGB(243, 246, 254), disabledFill = RGB(239, 243, 247);

inline COLORREF accent() {
    static const auto value = std::strtoul(msa::brand_blue + 1, nullptr, 16);
    return RGB(value >> 16, value >> 8, value);
}

inline bool highContrast() {
    HIGHCONTRASTW setting{sizeof(HIGHCONTRASTW)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(setting), &setting, 0) &&
           (setting.dwFlags & HCF_HIGHCONTRASTON);
}

inline void shapeColors(HDC dc, COLORREF outline, COLORREF fill) {
    SetDCPenColor(dc, outline);
    SetDCBrushColor(dc, fill);
}

// Keep native radio and pushbutton semantics for keyboard navigation and accessibility.
// Only their client-area painting changes. Sizes inside each DLU-scaled control use its DPI.
inline void paintButton(HWND button, HDC dc) {
    const auto dpi = GetDpiForWindow(button);
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const int id = GetDlgCtrlID(button);
    const bool radio = id == MSA_TWO_OPERATORS || id == MSA_THREE_OPERATORS;
    const bool checked = radio && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool enabled = IsWindowEnabled(button);
    const bool pressed = SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED;
    POINT cursor{};
    const bool hot = enabled && GetCursorPos(&cursor) && WindowFromPoint(cursor) == button;
    const bool primary = id == IDOK && enabled;
    RECT rect{};
    GetClientRect(button, &rect);
    const int previous = SaveDC(dc);
    SelectObject(dc, GetStockObject(DC_PEN));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, background);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    COLORREF fill = surface;
    if (!enabled)
        fill = disabledFill;
    else if (primary)
        fill = (hot || pressed) ? RGB(46, 83, 202) : accent();
    else if (pressed || hot)
        fill = hover;
    else if (checked)
        fill = selectedFill;
    shapeColors(dc, (checked || primary) ? accent() : border, fill);
    RoundRect(dc, 0, 0, rect.right, rect.bottom, px(16), px(16));

    wchar_t caption[128]{};
    GetWindowTextW(button, caption, 128);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, primary ? surface : (enabled ? ink : muted));
    constexpr UINT textStyle = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    HFONT heading = nullptr;
    if (radio) {
        const int x = px(16), y = (rect.bottom - px(18)) / 2, size = px(18);
        shapeColors(dc, checked ? accent() : muted, surface);
        Ellipse(dc, x, y, x + size, y + size);
        if (checked) {
            const int inset = px(4);
            shapeColors(dc, accent(), accent());
            Ellipse(dc, x + inset, y + inset, x + size - inset, y + size - inset);
        }
        wchar_t *detail = std::wcschr(caption, L'\n');
        if (detail)
            *detail++ = L'\0';
        RECT title{px(48), rect.bottom / 2 - px(24), rect.right - px(10), rect.bottom / 2};
        LOGFONTW description{};
        if (GetObjectW(font, sizeof(description), &description)) {
            description.lfWeight = FW_SEMIBOLD;
            description.lfHeight -= px(3);
            heading = CreateFontIndirectW(&description);
        }
        if (heading)
            SelectObject(dc, heading);
        DrawTextW(dc, caption, -1, &title, textStyle);
        if (detail) {
            SelectObject(dc, font);
            SetTextColor(dc, muted);
            RECT subtitle{title.left, title.bottom + px(1), title.right, title.bottom + px(23)};
            DrawTextW(dc, detail, -1, &subtitle, textStyle);
        }
    } else {
        DrawTextW(dc, caption, -1, &rect, textStyle | DT_CENTER);
    }
    if (GetFocus() == button) {
        InflateRect(&rect, -px(3), -px(3));
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetDCPenColor(dc, primary ? surface : accent());
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, px(12), px(12));
    }
    RestoreDC(dc, previous);
    if (heading)
        DeleteObject(heading);
}

inline LRESULT CALLBACK buttonProc(HWND button, UINT message, WPARAM wparam, LPARAM lparam,
                                   UINT_PTR subclassId, DWORD_PTR) {
    switch (message) {
    case WM_PAINT:
    case WM_PRINTCLIENT:
        if (!highContrast()) {
            if (message == WM_PAINT) {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(button, &paint);
                paintButton(button, dc);
                EndPaint(button, &paint);
            } else {
                paintButton(button, reinterpret_cast<HDC>(wparam));
            }
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_QUERY, button, 0};
        TrackMouseEvent(&tracking);
        if (!(tracking.dwFlags & TME_LEAVE)) {
            tracking = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, button, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(button, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case BM_SETCHECK:
    case BM_SETSTATE:
        InvalidateRect(button, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(button, buttonProc, subclassId);
        break;
    }
    return DefSubclassProc(button, message, wparam, lparam);
}

inline INT_PTR CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM) {
    switch (message) {
    case WM_INITDIALOG: {
        for (int id : {MSA_TWO_OPERATORS, MSA_THREE_OPERATORS, IDOK, IDCANCEL})
            SetWindowSubclass(GetDlgItem(window, id), buttonProc, 1, 0);
        const HWND first = GetDlgItem(window, MSA_TWO_OPERATORS);
        SetFocus(first);
        CheckDlgButton(window, MSA_TWO_OPERATORS, BST_UNCHECKED);
        // BM_SETCHECK clears the native auto-radio's tab stop when it is unchecked.
        SetWindowLongPtrW(first, GWL_STYLE, GetWindowLongPtrW(first, GWL_STYLE) | WS_TABSTOP);
        EnableWindow(GetDlgItem(window, IDOK), FALSE);
        return FALSE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        if (!highContrast()) {
            auto dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, muted);
            SetBkColor(dc, background);
            SetDCBrushColor(dc, background);
            return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case MSA_TWO_OPERATORS:
        case MSA_THREE_OPERATORS:
            if (HIWORD(wparam) == BN_CLICKED &&
                IsDlgButtonChecked(window, LOWORD(wparam)) == BST_CHECKED)
                EnableWindow(GetDlgItem(window, IDOK), TRUE);
            return TRUE;
        case IDOK:
            if (IsWindowEnabled(GetDlgItem(window, IDOK))) {
                if (IsDlgButtonChecked(window, MSA_TWO_OPERATORS) == BST_CHECKED)
                    EndDialog(window, 3);
                else if (IsDlgButtonChecked(window, MSA_THREE_OPERATORS) == BST_CHECKED)
                    EndDialog(window, 2);
            }
            return TRUE;
        case IDCANCEL:
            EndDialog(window, 0);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(window, 0);
        return TRUE;
    }
    return FALSE;
}
} // namespace platform::replicate_dialog
