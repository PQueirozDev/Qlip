#include "ui/TrayIcon.h"

#include <wchar.h>
#include <shellapi.h>

#include "ui/Theme.h"
#include "utils/Logger.h"

namespace {

constexpr UINT kIconId = 1;
constexpr UINT kMenuShow = 1;
constexpr UINT kMenuExit = 2;

}  // namespace

TrayIcon::TrayIcon(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

TrayIcon::~TrayIcon() {
    remove();
}

bool TrayIcon::create(HWND owner) {
    // Zera antes: se o re-add falhar, isAdded() não pode continuar dizendo que o ícone existe.
    added_ = false;
    data_.cbSize = sizeof(data_);
    data_.hWnd = owner;
    data_.uID = kIconId;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data_.uCallbackMessage = kMessage;
    data_.hIcon = theme::appIcon(GetSystemMetrics(SM_CXSMICON));
    wcscpy_s(data_.szTip, L"Qlip");

    if (!Shell_NotifyIconW(NIM_ADD, &data_)) {
        QLOG_ERROR(L"Falha ao adicionar o ícone da bandeja.");
        return false;
    }
    data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    added_ = true;
    return true;
}

void TrayIcon::remove() {
    if (!added_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
}

bool TrayIcon::handleMessage(LPARAM lParam) {
    switch (LOWORD(lParam)) {
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (callbacks_.onShow) callbacks_.onShow();
            return true;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            showMenu(data_.hWnd);
            return true;
    }
    return false;
}

void TrayIcon::showMenu(HWND owner) const {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, kMenuShow, L"Mostrar Qlip");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Sair");

    POINT cursor{};
    GetCursorPos(&cursor);
    // Necessário para o menu fechar ao clicar fora (limitação conhecida do Win32).
    SetForegroundWindow(owner);
    const UINT chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, owner, nullptr);
    // Sem esta mensagem, o menu pode não fechar/reabrir corretamente na próxima vez.
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (chosen == kMenuShow && callbacks_.onShow) callbacks_.onShow();
    if (chosen == kMenuExit && callbacks_.onExit) callbacks_.onExit();
}
