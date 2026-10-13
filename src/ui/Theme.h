#pragma once

#include <windows.h>

// Sem borda (valor de CLR_NONE do GDI, definido aqui caso o cabeçalho não o traga).
#ifndef CLR_NONE
#define CLR_NONE 0xFFFFFFFFu
#endif

// Identidade visual do Qlip: preto e azul, superfícies planas, bordas sutis, cantos arredondados.
// Cores e fontes ficam aqui para que todas as janelas usem a mesma paleta.
namespace theme {

inline constexpr COLORREF kBackground = RGB(0x09, 0x09, 0x0B);
inline constexpr COLORREF kSurface = RGB(0x11, 0x12, 0x16);
inline constexpr COLORREF kSurfaceHover = RGB(0x1A, 0x1C, 0x22);
inline constexpr COLORREF kInput = RGB(0x0D, 0x0E, 0x12);
inline constexpr COLORREF kBorder = RGB(0x1E, 0x21, 0x29);
inline constexpr COLORREF kAccent = RGB(0x3B, 0x82, 0xF6);
inline constexpr COLORREF kAccentPressed = RGB(0x25, 0x63, 0xEB);
inline constexpr COLORREF kText = RGB(0xF4, 0xF6, 0xFA);
inline constexpr COLORREF kTextMuted = RGB(0x8B, 0x93, 0xA1);
inline constexpr COLORREF kRecording = RGB(0xEF, 0x44, 0x44);
inline constexpr COLORREF kWarning = RGB(0xF5, 0x9E, 0x0B);

// Fonte Segoe UI com a altura em pixels (negativa na API) e peso.
HFONT createFont(int pixelHeight, int weight = FW_NORMAL);

// Retângulo arredondado com antialias (GDI+). border = CLR_NONE para sem borda.
void fillRoundRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border = CLR_NONE);

// Círculo preenchido com antialias.
void fillCircle(HDC dc, int cx, int cy, int radius, COLORREF fill);

// Ícone do Qlip (recurso embutido no exe) no tamanho pedido. Compartilhado: não precisa ser destruído.
HICON appIcon(int size);

// Barra de título escura (Windows 10 20H1+ e 11), combinando com o tema do app.
void applyDarkTitleBar(HWND window);

}  // namespace theme
