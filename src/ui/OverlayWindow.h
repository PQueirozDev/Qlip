#pragma once

#include <functional>
#include <string>

#include <windows.h>

// Painel por cima do jogo, aberto por atalho (como o Alt+Z do app da NVIDIA): escurece o monitor do jogo
// e mostra as ações do Qlip em blocos grandes. Fica fora das gravações (WDA_EXCLUDEFROMCAPTURE).
// Funciona com jogos em janela ou tela cheia sem borda; em tela cheia exclusiva o Windows pode
// minimizar o jogo ao trocar o foco.
class OverlayWindow {
public:
    struct Callbacks {
        std::function<void()> onToggleReplay;
        std::function<void()> onSaveClip;
        std::function<void()> onToggleRecording;
        std::function<void()> onOpenLibrary;
        std::function<void()> onOpenSettings;
        // Pedido de atualização do estado (ao abrir e a cada segundo enquanto visível).
        std::function<void()> onRefresh;
    };

    struct Status {
        bool replayOn = false;
        int replaySeconds = 30;
        bool recording = false;
        bool finalizing = false;
        unsigned long long recordingSeconds = 0;
        std::wstring notice;
        std::wstring keyReplay, keySave, keyRecord, keyOverlay;
    };

    OverlayWindow(HINSTANCE instance, Callbacks callbacks);
    ~OverlayWindow();

    void toggle();
    void show();
    // restoreFocus: devolve o foco à janela que estava na frente (o jogo) ao fechar.
    void hide(bool restoreFocus = true);
    bool isVisible() const;
    void setStatus(const Status& status);

private:
    enum Tile { kTileReplay, kTileSave, kTileRecord, kTileLibrary, kTileSettings, kTileCount };

    static LRESULT CALLBACK backdropProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK panelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handlePanel(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool ensureWindows();
    void layout(const RECT& monitor);
    void paint(HDC dc);
    void drawTile(HDC dc, int index) const;
    int tileAt(POINT point) const;
    void activate(int index);
    int px(int logical) const { return MulDiv(logical, dpi_, 96); }
    void recreateFonts();

    HINSTANCE instance_;
    Callbacks callbacks_;
    HWND backdrop_ = nullptr;
    HWND panel_ = nullptr;
    HWND previousForeground_ = nullptr;
    int dpi_ = 96;
    int fontsDpi_ = 0;
    int hovered_ = -1;
    int pressed_ = -1;
    bool tracking_ = false;
    RECT tiles_[kTileCount]{};
    Status status_;

    HFONT titleFont_ = nullptr;
    HFONT tileFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT iconFont_ = nullptr;
    HFONT chipFont_ = nullptr;
};
