#pragma once

#include <functional>

#include <windows.h>
#include <shellapi.h>

// Ícone na bandeja: duplo clique mostra a janela; botão direito abre o menu (Mostrar / Sair).
class TrayIcon {
public:
    struct Callbacks {
        std::function<void()> onShow;
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
