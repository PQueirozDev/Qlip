#include "ui/OverlayWindow.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include "ui/Theme.h"
#include "utils/Logger.h"

namespace {

constexpr wchar_t kBackdropClass[] = L"QlipOverlayBackdrop";
constexpr wchar_t kPanelClass[] = L"QlipOverlayPanel";
constexpr UINT_PTR kRefreshTimer = 1;

// Layout lógico (96 DPI); escalado pelo DPI do monitor do jogo.
constexpr int kPad = 24;
constexpr int kHeaderHeight = 72;
constexpr int kTileWidth = 168;
constexpr int kTileHeight = 148;
constexpr int kTileGap = 12;
constexpr int kFooterHeight = 52;
constexpr int kPanelWidth = kPad * 2 + 5 * kTileWidth + 4 * kTileGap;
constexpr int kPanelHeight = kHeaderHeight + kTileHeight + kFooterHeight;

// Exclui a janela de capturas de tela e gravações (Windows 10 2004+).
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

std::wstring hhmmss(unsigned long long seconds) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02llu:%02llu:%02llu", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    return buffer;
}

void drawText(HDC dc, HFONT font, COLORREF color, const std::wstring& text, RECT rect, UINT format) {
    HGDIOBJ previous = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous);
}

int textWidth(HDC dc, HFONT font, const std::wstring& text) {
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previous);
    return size.cx;
}

}  // namespace

OverlayWindow::OverlayWindow(HINSTANCE instance, Callbacks callbacks)
    : instance_(instance), callbacks_(std::move(callbacks)) {}

OverlayWindow::~OverlayWindow() {
    if (panel_) DestroyWindow(panel_);
    if (backdrop_) DestroyWindow(backdrop_);
    for (HFONT font : {titleFont_, tileFont_, smallFont_, iconFont_, chipFont_}) {
        if (font) DeleteObject(font);
    }
}

bool OverlayWindow::ensureWindows() {
    if (backdrop_ && panel_) return true;

    WNDCLASSEXW backdropClass{};
    backdropClass.cbSize = sizeof(backdropClass);
    backdropClass.lpfnWndProc = &OverlayWindow::backdropProc;
    backdropClass.hInstance = instance_;
    backdropClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    backdropClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    backdropClass.lpszClassName = kBackdropClass;
    RegisterClassExW(&backdropClass);

    WNDCLASSEXW panelClass{};
    panelClass.cbSize = sizeof(panelClass);
    panelClass.style = CS_DROPSHADOW;
    panelClass.lpfnWndProc = &OverlayWindow::panelProc;
    panelClass.hInstance = instance_;
    panelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    panelClass.lpszClassName = kPanelClass;
    RegisterClassExW(&panelClass);

    // Fundo escurecido: não recebe foco (clicar nele fecha o painel) e deixa o jogo visível por trás.
    backdrop_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kBackdropClass, L"",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, this);
    if (!backdrop_) {
        QLOG_ERROR(L"Falha ao criar o fundo do overlay.");
        return false;
    }
    SetLayeredWindowAttributes(backdrop_, 0, 150, LWA_ALPHA);

    // Painel: janela própria, dona do foco enquanto aberto (teclado e cliques).
    panel_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPanelClass, L"Qlip", WS_POPUP, 0, 0, 0, 0, backdrop_, nullptr,
                             instance_, this);
    if (!panel_) {
        QLOG_ERROR(L"Falha ao criar o painel do overlay.");
        DestroyWindow(backdrop_);
        backdrop_ = nullptr;
        return false;
    }

    // Overlay fora das gravações e clipes do próprio Qlip (e de outras capturas).
    const bool backdropExcluded = SetWindowDisplayAffinity(backdrop_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    const bool panelExcluded = SetWindowDisplayAffinity(panel_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    if (!backdropExcluded || !panelExcluded) {
        QLOG_WARN(L"Não foi possível excluir o overlay da captura (requer Windows 10 2004 ou mais novo).");
    }
    const int corner = 2;  // DWMWCP_ROUND (Windows 11; ignorado no 10)
    DwmSetWindowAttribute(panel_, 33, &corner, sizeof(corner));
    theme::applyDarkTitleBar(panel_);
    return true;
}

bool OverlayWindow::isVisible() const {
    return panel_ && IsWindowVisible(panel_);
}

void OverlayWindow::toggle() {
    if (isVisible()) {
        hide();
    } else {
        show();
    }
}

void OverlayWindow::show() {
    if (!ensureWindows()) return;

    // O painel abre no monitor do jogo (janela em primeiro plano) ou, sem ela, no do cursor.
    HWND foreground = GetForegroundWindow();
    previousForeground_ = (foreground && foreground != panel_ && foreground != backdrop_) ? foreground : nullptr;
    HMONITOR monitor = nullptr;
    if (previousForeground_) {
        monitor = MonitorFromWindow(previousForeground_, MONITOR_DEFAULTTOPRIMARY);
    } else {
        POINT cursor{};
        GetCursorPos(&cursor);
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);

    UINT dpiX = 96, dpiY = 96;
    dpi_ = SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) ? static_cast<int>(dpiX) : 96;
    recreateFonts();
    layout(info.rcMonitor);

    hovered_ = -1;
    pressed_ = -1;

    const RECT& m = info.rcMonitor;
    SetWindowPos(backdrop_, HWND_TOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const int width = px(kPanelWidth);
    const int height = px(kPanelHeight);
    const int x = m.left + ((m.right - m.left) - width) / 2;
    const int y = m.top + (m.bottom - m.top) / 9;
    SetWindowPos(panel_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
    // Estado pedido com o painel já visível (setStatus só repinta janela visível).
    if (callbacks_.onRefresh) callbacks_.onRefresh();
    SetForegroundWindow(panel_);
    SetFocus(panel_);
    SetTimer(panel_, kRefreshTimer, 1000, nullptr);
}

void OverlayWindow::hide(bool restoreFocus) {
    if (!panel_) return;
    KillTimer(panel_, kRefreshTimer);
    const bool wasVisible = IsWindowVisible(panel_) != FALSE;
    // Lido antes de esconder: esconder o painel gera WM_ACTIVATE, que chama hide(false) de novo.
    HWND previous = previousForeground_;
    previousForeground_ = nullptr;
    pressed_ = -1;
    if (GetCapture() == panel_) ReleaseCapture();
    ShowWindow(panel_, SW_HIDE);
    ShowWindow(backdrop_, SW_HIDE);
    // Devolve o foco ao jogo, como o overlay da NVIDIA.
    if (wasVisible && restoreFocus && previous && IsWindow(previous)) SetForegroundWindow(previous);
}

void OverlayWindow::setStatus(const Status& status) {
    status_ = status;
    if (isVisible()) InvalidateRect(panel_, nullptr, FALSE);
}

void OverlayWindow::recreateFonts() {
    if (fontsDpi_ == dpi_ && titleFont_) return;
    for (HFONT* font : {&titleFont_, &tileFont_, &smallFont_, &iconFont_, &chipFont_}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    titleFont_ = theme::createFont(px(20), FW_SEMIBOLD);
    tileFont_ = theme::createFont(px(15), FW_SEMIBOLD);
    smallFont_ = theme::createFont(px(13));
    chipFont_ = theme::createFont(px(12), FW_SEMIBOLD);
    // Ícones da fonte de sistema do Windows 10/11 (sem imagens embutidas).
    iconFont_ = CreateFontW(-px(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    fontsDpi_ = dpi_;
}

void OverlayWindow::layout(const RECT&) {
    for (int i = 0; i < kTileCount; ++i) {
        const int left = px(kPad + i * (kTileWidth + kTileGap));
        tiles_[i] = RECT{left, px(kHeaderHeight), left + px(kTileWidth), px(kHeaderHeight + kTileHeight)};
    }
}

int OverlayWindow::tileAt(POINT point) const {
    for (int i = 0; i < kTileCount; ++i) {
        if (PtInRect(&tiles_[i], point)) return i;
    }
    return -1;
}

void OverlayWindow::activate(int index) {
    switch (index) {
        case kTileReplay:
            if (callbacks_.onToggleReplay) callbacks_.onToggleReplay();
            break;
        case kTileSave:
            if (callbacks_.onSaveClip) callbacks_.onSaveClip();
            break;
        case kTileRecord:
            if (callbacks_.onToggleRecording) callbacks_.onToggleRecording();
            break;
        case kTileLibrary:
            // A janela aberta fica na frente: o foco não volta para o jogo.
            hide(false);
            if (callbacks_.onOpenLibrary) callbacks_.onOpenLibrary();
            break;
        case kTileSettings:
            hide(false);
            if (callbacks_.onOpenSettings) callbacks_.onOpenSettings();
            break;
        default:
            break;
    }
}

void OverlayWindow::drawTile(HDC dc, int index) const {
    const RECT& r = tiles_[index];
    const bool hover = hovered_ == index;
    const bool down = pressed_ == index && hover;
    const bool saveDisabled = index == kTileSave && !status_.replayOn;

    // Destaque do estado: replay ligado em azul, gravação em vermelho.
    COLORREF accent = theme::kText;
    COLORREF border = theme::kBorder;
    if (index == kTileReplay && status_.replayOn) accent = border = theme::kAccent;
    if (index == kTileRecord && (status_.recording || status_.finalizing)) accent = border = theme::kRecording;
    if (saveDisabled) accent = theme::kTextMuted;
    if (hover && border == theme::kBorder) border = RGB(0x2C, 0x31, 0x3C);

    const COLORREF fill = down ? theme::kInput : (hover ? theme::kSurfaceHover : theme::kSurface);
    theme::fillRoundRect(dc, r, px(12), fill, border);

    const wchar_t* icons[kTileCount] = {L"\xE81C", L"\xE896", status_.recording ? L"\xE71A" : L"\xE7C8", L"\xE8F1", L"\xE713"};
    std::wstring title, subtitle, key;
    switch (index) {
        case kTileReplay:
            title = L"Replay instantâneo";
            subtitle = status_.replayOn ? L"Ligado · últimos " + std::to_wstring(status_.replaySeconds) + L" s" : L"Desligado";
            key = status_.keyReplay;
            break;
        case kTileSave:
            title = L"Salvar clipe";
            subtitle = status_.replayOn ? L"Últimos " + std::to_wstring(status_.replaySeconds) + L" segundos" : L"Ligue o replay antes";
            key = status_.keySave;
            break;
        case kTileRecord:
            title = status_.recording ? L"Parar gravação" : L"Gravar";
            subtitle = status_.recording ? hhmmss(status_.recordingSeconds) : (status_.finalizing ? L"Finalizando…" : L"Gravação contínua");
            key = status_.keyRecord;
            break;
        case kTileLibrary:
            title = L"Biblioteca";
            subtitle = L"Gravações e clipes";
            break;
        default:
            title = L"Configurações";
            subtitle = L"Qualidade e atalhos";
            break;
    }

    const int pad = px(16);
    drawText(dc, iconFont_, accent, icons[index], RECT{r.left + pad, r.top + pad, r.right - pad, r.top + pad + px(30)}, DT_LEFT | DT_VCENTER);
    drawText(dc, tileFont_, saveDisabled ? theme::kTextMuted : theme::kText, title,
             RECT{r.left + pad, r.top + px(58), r.right - px(8), r.top + px(80)}, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    drawText(dc, smallFont_, index == kTileRecord && status_.recording ? theme::kText : theme::kTextMuted, subtitle,
             RECT{r.left + pad, r.top + px(82), r.right - px(8), r.top + px(102)}, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    if (!key.empty()) {
        // Tecla do atalho num "chip" no rodapé do bloco.
        const int width = textWidth(dc, chipFont_, key) + px(16);
        const RECT chip{r.left + pad, r.bottom - pad - px(22), r.left + pad + width, r.bottom - pad};
        theme::fillRoundRect(dc, chip, px(6), theme::kInput, theme::kBorder);
        drawText(dc, chipFont_, theme::kTextMuted, key, chip, DT_CENTER | DT_VCENTER);
    }
}

void OverlayWindow::paint(HDC target) {
    RECT client{};
    GetClientRect(panel_, &client);
    const int width = client.right;
    const int height = client.bottom;

    // Desenho em memória e cópia única: sem cintilação ao atualizar a cada segundo.
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);

    HBRUSH background = CreateSolidBrush(theme::kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);
    HBRUSH frame = CreateSolidBrush(theme::kBorder);
    FrameRect(dc, &client, frame);
    DeleteObject(frame);

    // Cabeçalho: ícone, nome e estado atual à direita.
    const int iconSize = px(28);
    DrawIconEx(dc, px(kPad), px(22), theme::appIcon(iconSize), iconSize, iconSize, 0, nullptr, DI_NORMAL);
    drawText(dc, titleFont_, theme::kText, L"Qlip", RECT{px(kPad + 38), px(18), px(300), px(54)}, DT_LEFT | DT_VCENTER);

    std::wstring state = L"Pronto";
    COLORREF dot = theme::kTextMuted;
    if (status_.recording) {
        state = L"Gravando  " + hhmmss(status_.recordingSeconds);
        dot = theme::kRecording;
    } else if (status_.finalizing) {
        state = L"Finalizando a gravação…";
        dot = theme::kWarning;
    } else if (status_.replayOn) {
        state = L"Replay ligado";
        dot = theme::kAccent;
    }
    const int stateWidth = textWidth(dc, smallFont_, state);
    const int stateRight = width - px(kPad);
    drawText(dc, smallFont_, theme::kText, state, RECT{stateRight - stateWidth, px(18), stateRight, px(54)}, DT_LEFT | DT_VCENTER);
    theme::fillCircle(dc, stateRight - stateWidth - px(12), px(36), px(4), dot);

    for (int i = 0; i < kTileCount; ++i) drawTile(dc, i);

    // Rodapé: aviso recente (ex.: "Clipe salvo") ou dica; à direita, como fechar.
    const int footerTop = px(kHeaderHeight + kTileHeight);
    const std::wstring hint = status_.keyOverlay + L" ou Esc para fechar";
    drawText(dc, smallFont_, status_.notice.empty() ? theme::kTextMuted : theme::kText,
             status_.notice.empty() ? L"Clique em uma ação ou use os atalhos sem sair do jogo." : status_.notice,
             RECT{px(kPad), footerTop, width - px(260), height}, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    drawText(dc, smallFont_, theme::kTextMuted, hint, RECT{width - px(260), footerTop, width - px(kPad), height}, DT_RIGHT | DT_VCENTER);

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT CALLBACK OverlayWindow::backdropProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<OverlayWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    // Clique fora do painel fecha o overlay e devolve o foco ao jogo.
    if (self && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN)) {
        self->hide();
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK OverlayWindow::panelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<OverlayWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handlePanel(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT OverlayWindow::handlePanel(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                tracking_ = TrackMouseEvent(&track) != FALSE;
            }
            const int hit = tileAt(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hit != hovered_) {
                hovered_ = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            hovered_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && hovered_ >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN:
            pressed_ = tileAt(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (pressed_ >= 0) SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONUP: {
            const int hit = tileAt(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            const int pressed = pressed_;
            pressed_ = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (hit >= 0 && hit == pressed) activate(hit);
            return 0;
        }

        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            // Clique abandonado (captura perdida): soltar o botão depois não aciona nada.
            pressed_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (msg == WM_CANCELMODE && GetCapture() == hwnd) ReleaseCapture();
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                hide();
                return 0;
            }
            break;

        case WM_ACTIVATE:
            // Outra janela ganhou o foco (Alt+Tab, clique em outro monitor): o painel some sem roubar o foco de volta.
            if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible(hwnd)) hide(false);
            return 0;

        case WM_TIMER:
            if (wParam == kRefreshTimer && callbacks_.onRefresh) callbacks_.onRefresh();
            return 0;

        case WM_CLOSE:
            hide();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
