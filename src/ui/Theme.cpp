#include "ui/Theme.h"

#include <mutex>

#include <dwmapi.h>

#include "resource.h"

#include <objidl.h>  // IStream, exigido pelo cabeçalho do GDI+
#include <gdiplus.h>

namespace theme {

namespace {

// GDI+ é inicializado uma vez por processo (usado só para desenho com antialias).
void ensureGdiPlus() {
    static std::once_flag once;
    static ULONG_PTR token = 0;
    std::call_once(once, [] {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
    });
}

Gdiplus::Color toGdi(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

// Caminho de um retângulo com cantos arredondados.
void addRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    const float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

}  // namespace

HFONT createFont(int pixelHeight, int weight) {
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void fillRoundRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    ensureGdiPlus();
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // Meio pixel de inset para a borda ficar nítida dentro do retângulo.
    Gdiplus::RectF r(static_cast<float>(rect.left) + 0.5f, static_cast<float>(rect.top) + 0.5f,
                     static_cast<float>(rect.right - rect.left) - 1.0f, static_cast<float>(rect.bottom - rect.top) - 1.0f);
    Gdiplus::GraphicsPath path;
    addRoundedRect(path, r, static_cast<float>(radius));

    Gdiplus::SolidBrush brush(toGdi(fill));
    graphics.FillPath(&brush, &path);
    if (border != CLR_NONE) {
        Gdiplus::Pen pen(toGdi(border), 1.0f);
        graphics.DrawPath(&pen, &path);
    }
}

void applyDarkTitleBar(HWND window) {
    // DWMWA_USE_IMMERSIVE_DARK_MODE (20). Em versões sem suporte a chamada só falha, sem efeito.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
}

HICON appIcon(int size) {
    HANDLE icon = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_QCLIP), IMAGE_ICON, size, size,
                             LR_DEFAULTCOLOR | LR_SHARED);
    return icon ? static_cast<HICON>(icon) : LoadIconW(nullptr, IDI_APPLICATION);
}

void fillCircle(HDC dc, int cx, int cy, int radius, COLORREF fill) {
    ensureGdiPlus();
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(toGdi(fill));
    graphics.FillEllipse(&brush, cx - radius, cy - radius, radius * 2, radius * 2);
}

}  // namespace theme
