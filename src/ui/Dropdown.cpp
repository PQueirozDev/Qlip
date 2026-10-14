#include "ui/Dropdown.h"

#include <algorithm>

#include <dwmapi.h>
#include <windowsx.h>

#include "ui/Theme.h"

namespace {

constexpr wchar_t kDropdownClass[] = L"QlipDropdown";
constexpr int kItemHeight = 30;
constexpr int kInset = 4;
constexpr int kMaxVisible = 10;

struct DropdownState {
    const std::vector<std::wstring>* items = nullptr;
    HFONT font = nullptr;
    int current = -1;
    int hover = -1;
    int chosen = -1;
    int first = 0;    // primeiro item visível (rolagem)
    int visible = 0;  // quantos itens cabem na lista
    bool done = false;

    int count() const { return static_cast<int>(items->size()); }
    void scrollTo(int index) {
        if (index < first) first = index;
        if (index >= first + visible) first = index - visible + 1;
        first = std::clamp(first, 0, std::max(0, count() - visible));
    }
};

int itemAt(HWND hwnd, const DropdownState& state, POINT point) {
    RECT client{};
    GetClientRect(hwnd, &client);
    if (!PtInRect(&client, point)) return -1;
    if (point.y < kInset) return -1;
    const int row = (point.y - kInset) / kItemHeight;
    const int index = state.first + row;
    return row < state.visible && index < state.count() ? index : -1;
}

void paint(HWND hwnd, HDC target, const DropdownState& state) {
    RECT client{};
    GetClientRect(hwnd, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    HBRUSH background = CreateSolidBrush(theme::kSurface);
    FillRect(dc, &client, background);
    DeleteObject(background);
    HBRUSH frame = CreateSolidBrush(theme::kBorder);
    FrameRect(dc, &client, frame);
    DeleteObject(frame);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, state.font);
    const bool scrolls = state.count() > state.visible;
    const int right = client.right - kInset - (scrolls ? 6 : 0);
    for (int r = 0; r < state.visible; ++r) {
        const int i = state.first + r;
        const RECT row{kInset, kInset + r * kItemHeight, right, kInset + (r + 1) * kItemHeight};
        if (i == state.hover) theme::fillRoundRect(dc, row, 6, theme::kSurfaceHover);
        if (i == state.current) {
            // Opção atual: barra azul à esquerda.
            const RECT bar{row.left + 4, row.top + 8, row.left + 7, row.bottom - 8};
            theme::fillRoundRect(dc, bar, 1, theme::kAccent);
        }
        RECT text{row.left + 14, row.top, row.right - 10, row.bottom};
        SetTextColor(dc, i == state.current ? theme::kText : RGB(0xC9, 0xCE, 0xD8));
        DrawTextW(dc, (*state.items)[i].c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);

    if (scrolls) {
        // Indicador de rolagem: trilho discreto com a posição atual.
        const int track = client.bottom - kInset * 2;
        const int thumb = std::max(16, track * state.visible / state.count());
        const int top = kInset + (track - thumb) * state.first / std::max(1, state.count() - state.visible);
        const RECT bar{client.right - kInset - 4, top, client.right - kInset, top + thumb};
        theme::fillRoundRect(dc, bar, 2, theme::kBorder);
    }

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT CALLBACK dropdownProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* state = reinterpret_cast<DropdownState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            paint(hwnd, dc, *state);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            // Com captura, o ponto pode estar fora da lista: aí nenhum item fica destacado.
            const int hit = itemAt(hwnd, *state, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hit != state->hover && hit >= 0) {
                state->hover = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            // Clique fora fecha sem escolher (inclusive no próprio seletor, como um combo nativo).
            RECT client{};
            GetClientRect(hwnd, &client);
            if (!PtInRect(&client, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) state->done = true;
            return 0;
        }
        case WM_LBUTTONUP: {
            const int hit = itemAt(hwnd, *state, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (hit >= 0) {
                state->chosen = hit;
                state->done = true;
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            state->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int showDropdown(HWND owner, const RECT& anchor, const std::vector<std::wstring>& items, int current, HFONT font) {
    if (items.empty()) return -1;

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = dropdownProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kDropdownClass;
    RegisterClassExW(&wc);  // falha silenciosa se já registrada

    DropdownState state;
    state.items = &items;
    state.font = font;
    state.current = current;
    state.hover = current;

    // Largura: a do seletor, ou a do texto mais longo se for maior.
    HDC screen = GetDC(nullptr);
    HGDIOBJ oldFont = SelectObject(screen, font);
    int width = anchor.right - anchor.left;
    for (const std::wstring& item : items) {
        SIZE size{};
        GetTextExtentPoint32W(screen, item.c_str(), static_cast<int>(item.size()), &size);
        width = std::max(width, static_cast<int>(size.cx) + 40);
    }
    SelectObject(screen, oldFont);
    ReleaseDC(nullptr, screen);

    // Abre abaixo do seletor; sem espaço, acima. Sempre dentro da área de trabalho do monitor.
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &info);
    const RECT& work = info.rcWork;
    width = std::min(width, static_cast<int>(work.right - work.left));
    // Listas longas mostram até kMaxVisible itens e rolam (roda do mouse e setas).
    const int maxFit = std::max(1, static_cast<int>((work.bottom - work.top) / 2 - kInset * 2) / kItemHeight);
    state.visible = std::min({static_cast<int>(items.size()), kMaxVisible, maxFit});
    state.first = std::clamp(current - state.visible + 1, 0, static_cast<int>(items.size()) - state.visible);
    const int height = state.visible * kItemHeight + kInset * 2;
    int x = std::clamp(static_cast<int>(anchor.left), static_cast<int>(work.left), static_cast<int>(work.right) - width);
    int y = anchor.bottom + 4;
    if (y + height > work.bottom) y = std::max(static_cast<int>(work.top), static_cast<int>(anchor.top) - 4 - height);

    HWND root = GetAncestor(owner, GA_ROOT);
    HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kDropdownClass, L"", WS_POPUP, x, y, width,
                                 height, root, nullptr, instance, &state);
    if (!popup) return -1;
    const int corner = 3;  // DWMWCP_ROUNDSMALL (Windows 11)
    DwmSetWindowAttribute(popup, 33, &corner, sizeof(corner));
    ShowWindow(popup, SW_SHOWNOACTIVATE);
    SetCapture(popup);

    // Laço próprio, como o de um menu: o teclado é tratado aqui, o resto segue para as janelas.
    while (!state.done) {
        MSG msg{};
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));  // repassa o pedido de saída ao laço principal
            break;
        }
        if (result == -1) break;

        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            const int count = static_cast<int>(items.size());
            switch (msg.wParam) {
                case VK_UP:
                    state.hover = state.hover <= 0 ? 0 : state.hover - 1;
                    break;
                case VK_DOWN:
                    state.hover = state.hover < 0 ? 0 : std::min(count - 1, state.hover + 1);
                    break;
                case VK_HOME:
                    state.hover = 0;
                    break;
                case VK_END:
                    state.hover = count - 1;
                    break;
                case VK_RETURN:
                case VK_SPACE:
                    state.chosen = state.hover;
                    state.done = true;
                    break;
                default:
                    // Esc, Tab, Alt e qualquer outra tecla fecham sem escolher.
                    state.done = true;
                    break;
            }
            if (state.hover >= 0) state.scrollTo(state.hover);
            InvalidateRect(popup, nullptr, FALSE);
            continue;
        }
        if (msg.message == WM_MOUSEWHEEL) {
            const int steps = GET_WHEEL_DELTA_WPARAM(msg.wParam) / WHEEL_DELTA;
            state.first = std::clamp(state.first - steps, 0, std::max(0, state.count() - state.visible));
            InvalidateRect(popup, nullptr, FALSE);
            continue;
        }
        if (msg.message == WM_CHAR || msg.message == WM_SYSCHAR || msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) continue;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // Outra janela tomou a captura ou o foco (Alt+Tab): fecha.
        if (!state.done && (GetCapture() != popup || GetForegroundWindow() != root)) state.done = true;
    }

    if (GetCapture() == popup) ReleaseCapture();
    DestroyWindow(popup);
    return state.chosen;
}
