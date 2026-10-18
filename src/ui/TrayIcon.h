#pragma once

#include <functional>

#include <windows.h>
#include <shellapi.h>

// Ícone na bandeja: clique abre o painel; botão direito abre o menu (painel, configurações, biblioteca, sair).
class TrayIcon {
public:
    struct Callbacks {
        std::function<void()> onShow;  // abre o painel (o mesmo do Alt+P)
        std::function<void()> onSettings;
        std::function<void()> onLibrary;
        std::function<void()> onExit;
    };

    explicit TrayIcon(Callbacks callbacks);
    ~TrayIcon();

    // Também usado para recriar o ícone quando o Explorer reinicia (mensagem TaskbarCreated).
    bool create(HWND owner);
    void remove();
    bool isAdded() const { return added_; }

    // Retorna true se a mensagem era do ícone e foi tratada.
    bool handleMessage(LPARAM lParam);

    static constexpr UINT kMessage = WM_APP + 1;

private:
    void showMenu(HWND owner) const;

    Callbacks callbacks_;
    NOTIFYICONDATAW data_{};
    bool added_ = false;
};
