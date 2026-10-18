#include "ui/ToastWindow.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include "ui/Theme.h"

namespace {

constexpr wchar_t kToastClass[] = L"QlipToast";
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT kVisibleMs = 2800;
constexpr int kWidth = 340;
constexpr int kHeight = 52;
constexpr int kMargin = 24;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

}  // namespace

ToastWindow::ToastWindow(HINSTANCE instance) : instance_(instance) {}

ToastWindow::~ToastWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
}

bool ToastWindow::ensureWindow() {
    if (hwnd_) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ToastWindow::wndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kToastClass;
    RegisterClassExW(&wc);

    // Transparente a cliques e sem ativação: o jogo continua com o foco e o mouse.
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                            kToastClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, 0, 240, LWA_ALPHA);
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    const int corner = 2;  // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));
    return true;
}

void ToastWindow::show(const std::wstring& text, COLORREF dot) {
    if (!ensureWindow()) return;
    text_ = text;
    dot_ = dot;

    // Canto superior direito do monitor do jogo (janela em primeiro plano).
    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = MonitorFromWindow(foreground ? foreground : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    UINT dpiX = 96, dpiY = 96;
    dpi_ = SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) ? static_cast<int>(dpiX) : 96;
    if (fontDpi_ != dpi_) {
        if (font_) DeleteObject(font_);
        font_ = theme::createFont(px(14), FW_SEMIBOLD);
        fontDpi_ = dpi_;
    }

    const RECT& m = info.rcMonitor;
    const int width = px(kWidth);
    SetWindowPos(hwnd_, HWND_TOPMOST, m.right - width - px(kMargin), m.top + px(kMargin), width, px(kHeight),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
    SetTimer(hwnd_, kHideTimer, kVisibleMs, nullptr);
}

void ToastWindow::hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kHideTimer);
    ShowWindow(hwnd_, SW_HIDE);
}

void ToastWindow::paint(HDC target) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    HBRUSH background = CreateSolidBrush(theme::kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);
    HBRUSH frame = CreateSolidBrush(theme::kBorder);
    FrameRect(dc, &client, frame);
    DeleteObject(frame);

    const int iconSize = px(22);
    const int iconTop = (client.bottom - iconSize) / 2;
    DrawIconEx(dc, px(14), iconTop, theme::appIcon(iconSize), iconSize, iconSize, 0, nullptr, DI_NORMAL);
    theme::fillCircle(dc, px(50), client.bottom / 2, px(4), dot_);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme::kText);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    RECT text{px(62), 0, client.right - px(14), client.bottom};
    DrawTextW(dc, text_.c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT CALLBACK ToastWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ToastWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<ToastWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ToastWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC dc = BeginPaint(hwnd, &ps);
                self->paint(dc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_TIMER:
                if (wParam == kHideTimer) self->hide();
                return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
