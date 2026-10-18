#pragma once

#include <string>

#include <windows.h>

// Aviso rápido no canto do monitor do jogo ("Clipe salvo", "Replay ligado"), como o da NVIDIA.
// Não pega foco nem cliques, some sozinho e fica fora das gravações (WDA_EXCLUDEFROMCAPTURE).
class ToastWindow {
public:
    explicit ToastWindow(HINSTANCE instance);
    ~ToastWindow();

    // Mostra (ou troca) o aviso por alguns segundos. dot: cor da bolinha à esquerda.
    void show(const std::wstring& text, COLORREF dot);
    void hide();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool ensureWindow();
    void paint(HDC target);
    int px(int logical) const { return MulDiv(logical, dpi_, 96); }

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    int fontDpi_ = 0;
    int dpi_ = 96;
    std::wstring text_;
    COLORREF dot_ = 0;
};
