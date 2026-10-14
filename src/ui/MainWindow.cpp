#include "ui/MainWindow.h"

#include "ui/Theme.h"
#include "ui/TrayIcon.h"
#include "utils/Logger.h"

namespace {

constexpr wchar_t kWindowClass[] = L"QlipMainWindow";
constexpr UINT_PTR kControlIdRecord = 1001;
constexpr UINT_PTR kControlIdSave = 1002;
constexpr UINT_PTR kControlIdSettings = 1003;

// Layout em pixels da área cliente.
constexpr int kClientWidth = 380;
constexpr int kClientHeight = 420;
constexpr UINT_PTR kControlIdLibrary = 1004;
const RECT kStatusCard{20, 84, 360, 244};
const RECT kRecordRect{20, 262, 360, 308};
const RECT kSaveRect{20, 318, 180, 356};
const RECT kSettingsRect{200, 318, 360, 356};
const RECT kLibraryRect{20, 366, 360, 402};

}  // namespace

MainWindow::MainWindow(Callbacks callbacks) : callbacks_(std::move(callbacks)) {
    backgroundBrush_ = CreateSolidBrush(theme::kBackground);
    cardBrush_ = CreateSolidBrush(theme::kSurface);
}

MainWindow::~MainWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    for (HFONT font : {titleFont_, statusFont_, timerFont_, bodyFont_, buttonFont_}) {
        if (font) DeleteObject(font);
    }
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
    if (cardBrush_) DeleteObject(cardBrush_);
}

bool MainWindow::create(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWindow::wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = backgroundBrush_;
    wc.hIcon = theme::appIcon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = theme::appIcon(GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        QLOG_ERROR(L"Falha ao registrar a classe da janela principal.");
        return false;
    }

    // Tamanho externo calculado para que a área cliente seja exatamente kClientWidth x kClientHeight.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT frame{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRect(&frame, style, FALSE);

    hwnd_ = CreateWindowExW(0, kWindowClass, L"Qlip", style,
                            CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        QLOG_ERROR(L"Falha ao criar a janela principal.");
        return false;
    }
    theme::applyDarkTitleBar(hwnd_);

    titleFont_ = theme::createFont(22, FW_SEMIBOLD);
    statusFont_ = theme::createFont(17, FW_SEMIBOLD);
    timerFont_ = theme::createFont(42, FW_LIGHT);
    bodyFont_ = theme::createFont(13);
    buttonFont_ = theme::createFont(14, FW_SEMIBOLD);

    // Cabeçalho: nome do app e subtítulo.
    HWND title = CreateWindowExW(0, L"STATIC", L"Qlip", WS_CHILD | WS_VISIBLE, 24, 20, 200, 30, hwnd_, nullptr, instance, nullptr);
    HWND subtitle = CreateWindowExW(0, L"STATIC", L"Gravador de tela e clipes", WS_CHILD | WS_VISIBLE, 24, 52, 320, 20, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(2000)), instance, nullptr);

    // Card de status: estado, cronômetro e detalhe.
    statusTitle_ = CreateWindowExW(0, L"STATIC", L"Parado", WS_CHILD | WS_VISIBLE, 60, 100, 280, 24, hwnd_, nullptr, instance, nullptr);
    timerLabel_ = CreateWindowExW(0, L"STATIC", L"00:00:00", WS_CHILD | WS_VISIBLE, 40, 132, 300, 56, hwnd_, nullptr, instance, nullptr);
    statusDetail_ = CreateWindowExW(0, L"STATIC", L"Pressione F9 para gravar", WS_CHILD | WS_VISIBLE, 44, 200, 300, 22, hwnd_,
                                    nullptr, instance, nullptr);

    // Botões desenhados pelo proprietário (WM_DRAWITEM) para seguir o tema.
    recordButton_ = CreateWindowExW(0, L"BUTTON", L"Gravar  ·  F9", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    kRecordRect.left, kRecordRect.top, kRecordRect.right - kRecordRect.left, kRecordRect.bottom - kRecordRect.top,
                                    hwnd_, reinterpret_cast<HMENU>(kControlIdRecord), instance, nullptr);
    saveButton_ = CreateWindowExW(0, L"BUTTON", L"Salvar clipe  ·  F8", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  kSaveRect.left, kSaveRect.top, kSaveRect.right - kSaveRect.left, kSaveRect.bottom - kSaveRect.top,
                                  hwnd_, reinterpret_cast<HMENU>(kControlIdSave), instance, nullptr);
    settingsButton_ = CreateWindowExW(0, L"BUTTON", L"Configurações", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      kSettingsRect.left, kSettingsRect.top, kSettingsRect.right - kSettingsRect.left,
                                      kSettingsRect.bottom - kSettingsRect.top, hwnd_, reinterpret_cast<HMENU>(kControlIdSettings), instance, nullptr);

    libraryButton_ = CreateWindowExW(0, L"BUTTON", L"Biblioteca de gravações e clipes", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     kLibraryRect.left, kLibraryRect.top, kLibraryRect.right - kLibraryRect.left,
                                     kLibraryRect.bottom - kLibraryRect.top, hwnd_, reinterpret_cast<HMENU>(kControlIdLibrary), instance, nullptr);
    SendMessageW(libraryButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
    SendMessageW(subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    SendMessageW(statusTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(statusFont_), TRUE);
    SendMessageW(timerLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(timerFont_), TRUE);
    SendMessageW(statusDetail_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    SendMessageW(recordButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
    SendMessageW(saveButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
    SendMessageW(settingsButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
    return true;
}

void MainWindow::show() {
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void MainWindow::hideToTray() {
    ShowWindow(hwnd_, SW_HIDE);
}

void MainWindow::setState(RecordState state) {
    if (state_ == state) return;
    state_ = state;
    // A bolinha de estado é desenhada no fundo: repinta a área do card.
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::setStatus(const std::wstring& title) {
    SetWindowTextW(statusTitle_, title.c_str());
}

void MainWindow::setDetail(const std::wstring& text) {
    SetWindowTextW(statusDetail_, text.c_str());
}

void MainWindow::setTimer(const std::wstring& text) {
    SetWindowTextW(timerLabel_, text.c_str());
}

void MainWindow::setRecording(bool recording) {
    SetWindowTextW(recordButton_, recording ? L"Parar  ·  F9" : L"Gravar  ·  F9");
    InvalidateRect(recordButton_, nullptr, FALSE);
}

void MainWindow::setTicking(bool ticking) {
    if (ticking) {
        SetTimer(hwnd_, kTimerId, 1000, nullptr);
    } else {
        KillTimer(hwnd_, kTimerId);
    }
}

void MainWindow::postRecordingEvent() {
    // Chamado de outras threads: PostMessage é seguro entre threads.
    if (hwnd_) PostMessageW(hwnd_, kMsgRecordingEvent, 0, 0);
}

void MainWindow::drawBackground(HDC dc, const RECT& client) const {
    FillRect(dc, &client, backgroundBrush_);
    theme::fillRoundRect(dc, kStatusCard, 14, theme::kSurface, theme::kBorder);

    // Bolinha de estado: vermelha gravando, âmbar finalizando, cinza parado.
    COLORREF dot = theme::kTextMuted;
    if (state_ == RecordState::Recording) dot = theme::kRecording;
    if (state_ == RecordState::Finalizing) dot = theme::kWarning;
    if (state_ == RecordState::Replay) dot = theme::kAccent;
    theme::fillCircle(dc, 46, 112, 6, dot);
}

void MainWindow::drawButton(const DRAWITEMSTRUCT& item) const {
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = item.CtlID == kControlIdRecord;

    RECT rect = item.rcItem;
    COLORREF fill = primary ? (pressed ? theme::kAccentPressed : theme::kAccent)
                            : (pressed ? theme::kSurfaceHover : theme::kSurface);
    COLORREF border = primary ? CLR_NONE : theme::kBorder;

    // O fundo do botão é o da janela: sem isso o canto arredondado deixa rastro ao repintar.
    FillRect(item.hDC, &rect, backgroundBrush_);
    theme::fillRoundRect(item.hDC, rect, 10, fill, border);

    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, 64);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, theme::kText);
    HFONT previous = static_cast<HFONT>(SelectObject(item.hDC, buttonFont_));
    DrawTextW(item.hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, previous);
}

LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (msg == TrayIcon::kMessage) {
        if (callbacks_.onTrayEvent) callbacks_.onTrayEvent(lParam);
        return 0;
    }
    if (msg == taskbarCreated) {
        if (callbacks_.onTaskbarRecreated) callbacks_.onTaskbarRecreated();
        return 0;
    }
    static const UINT exitRequest = RegisterWindowMessageW(kExitMessageName);
    if (msg == exitRequest) {
        if (callbacks_.onExitRequested) callbacks_.onExitRequested();
        return 0;
    }

    switch (msg) {
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            drawBackground(reinterpret_cast<HDC>(wParam), client);
            return 1;
        }

        case WM_CTLCOLORSTATIC: {
            // Textos do card usam o fundo do card; os do cabeçalho, o fundo da janela.
            HDC dc = reinterpret_cast<HDC>(wParam);
            const HWND control = reinterpret_cast<HWND>(lParam);
            const bool onCard = control == statusTitle_ || control == timerLabel_ || control == statusDetail_;
            SetBkColor(dc, onCard ? theme::kSurface : theme::kBackground);
            SetTextColor(dc, control == statusDetail_ ? theme::kTextMuted : theme::kText);
            return reinterpret_cast<LRESULT>(onCard ? cardBrush_ : backgroundBrush_);
        }

        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON) {
                drawButton(*item);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == kControlIdRecord) {
                if (callbacks_.onRecordClicked) callbacks_.onRecordClicked();
            } else if (LOWORD(wParam) == kControlIdSettings) {
                if (callbacks_.onSettingsClicked) callbacks_.onSettingsClicked();
            } else if (LOWORD(wParam) == kControlIdSave) {
                if (callbacks_.onSaveClipClicked) callbacks_.onSaveClipClicked();
            } else if (LOWORD(wParam) == kControlIdLibrary) {
                if (callbacks_.onLibraryClicked) callbacks_.onLibraryClicked();
            }
            return 0;

        case WM_HOTKEY:
            if (callbacks_.onHotkey) callbacks_.onHotkey(static_cast<int>(wParam));
            return 0;

        case WM_TIMER:
            // Atualiza o cronômetro só quando a janela está visível (sem custo com a janela escondida).
            if (wParam == kTimerId && IsWindowVisible(hwnd) && callbacks_.onTimer) callbacks_.onTimer();
            return 0;

        case kMsgRecordingEvent:
            if (callbacks_.onRecordingEvent) callbacks_.onRecordingEvent();
            return 0;

        case WM_CLOSE:
            // Fechar a janela só esconde se a bandeja estiver disponível; senão encerra o app.
            if (callbacks_.canHideToTray && callbacks_.canHideToTray()) {
                hideToTray();
            } else {
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
            hwnd_ = nullptr;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
