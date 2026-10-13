#pragma once

#include <atomic>
#include <functional>
#include <string>

#include <windows.h>

// Janela principal compacta: cabeçalho, card de status com cronômetro e botões de ação.
class MainWindow {
public:
    struct Callbacks {
        // Recebe o lParam das mensagens do ícone da bandeja (TrayIcon::kMessage).
        std::function<void(LPARAM)> onTrayEvent;
        // Chamado quando o Explorer reinicia e o ícone precisa ser recriado.
        std::function<void()> onTaskbarRecreated;
        // Fechar a janela só esconde se retornar true (bandeja disponível); senão o app encerra.
        std::function<bool()> canHideToTray;
        // Botão de gravação (mesma ação do F9).
        std::function<void()> onRecordClicked;
        // Botão de configurações.
        std::function<void()> onSettingsClicked;
        // Botão de salvar clipe (mesma ação do F8).
        std::function<void()> onSaveClipClicked;
        // Botão da biblioteca de gravações e clipes.
        std::function<void()> onLibraryClicked;
        // Atalho global registrado com RegisterHotKey; recebe o id registrado.
        std::function<void(int id)> onHotkey;
        // Atualização de 1 segundo enquanto a janela está visível.
        std::function<void()> onTimer;
        // Evento vindo de outra thread (fim da finalização ou falha da gravação).
        std::function<void()> onRecordingEvent;
    };

    enum class RecordState { Idle, Recording, Finalizing, Replay };

    // Mensagem postada por outras threads para avisar a UI sobre a gravação.
    static constexpr UINT kMsgRecordingEvent = WM_APP + 2;

    explicit MainWindow(Callbacks callbacks);
    ~MainWindow();

    bool create(HINSTANCE instance);
    void show();
    void hideToTray();

    void setState(RecordState state);
    void setStatus(const std::wstring& title);
    void setDetail(const std::wstring& text);
    void setTimer(const std::wstring& text);
    // Troca o texto do botão principal.
    void setRecording(bool recording);
    // Liga/desliga a atualização de 1 s (cronômetro e buffer do replay).
    void setTicking(bool ticking);
    void postRecordingEvent();

    HWND handle() const { return hwnd_; }

private:
    static constexpr UINT_PTR kTimerId = 1;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void layoutControls();
    void drawBackground(HDC dc, const RECT& client) const;
    void drawButton(const DRAWITEMSTRUCT& item) const;

    Callbacks callbacks_;
    // Atomic: postRecordingEvent() lê o handle de outra thread enquanto a janela pode estar sendo destruída.
    std::atomic<HWND> hwnd_{nullptr};
    HWND statusTitle_ = nullptr;
    HWND statusDetail_ = nullptr;
    HWND timerLabel_ = nullptr;
    HWND recordButton_ = nullptr;
    HWND saveButton_ = nullptr;
    HWND settingsButton_ = nullptr;
    HWND libraryButton_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT statusFont_ = nullptr;
    HFONT timerFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT buttonFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    RecordState state_ = RecordState::Idle;
};
