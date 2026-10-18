#include "ui/LibraryWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
}

#include "ui/Theme.h"
#include "utils/Logger.h"
#include "utils/Text.h"

namespace {

constexpr wchar_t kWindowClass[] = L"QlipLibraryWindow";
constexpr UINT_PTR kIdList = 4001;
constexpr UINT_PTR kIdOpen = 4002;
constexpr UINT_PTR kIdFolder = 4003;
constexpr UINT_PTR kIdDelete = 4004;
constexpr UINT_PTR kIdRefresh = 4005;

constexpr int kClientWidth = 720;
constexpr int kClientHeight = 470;
constexpr COLORREF kInput = theme::kInput;

// Duração lida do cabeçalho do arquivo (rápido: não decodifica vídeo). 0 se não for possível ler.
double mediaDurationSeconds(const std::filesystem::path& path) {
    AVFormatContext* context = nullptr;
    const std::string utf8 = toUtf8(path.wstring());
    if (avformat_open_input(&context, utf8.c_str(), nullptr, nullptr) < 0) return 0.0;
    double seconds = 0.0;
    if (avformat_find_stream_info(context, nullptr) >= 0 && context->duration > 0) {
        seconds = static_cast<double>(context->duration) / AV_TIME_BASE;
    }
    avformat_close_input(&context);
    return seconds;
}

std::wstring formatDuration(double seconds) {
    const auto total = static_cast<unsigned long long>(seconds + 0.5);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02llu:%02llu:%02llu", total / 3600, (total / 60) % 60, total % 60);
    return buffer;
}

std::wstring formatSize(uint64_t bytes) {
    wchar_t buffer[32]{};
    if (bytes >= 1024ull * 1024 * 1024) {
        swprintf_s(buffer, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else {
        swprintf_s(buffer, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buffer;
}

std::wstring formatDate(int64_t fileTime) {
    FILETIME utc{};
    utc.dwLowDateTime = static_cast<DWORD>(fileTime & 0xFFFFFFFF);
    utc.dwHighDateTime = static_cast<DWORD>(fileTime >> 32);
    FILETIME local{};
    FileTimeToLocalFileTime(&utc, &local);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&local, &st);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02d/%02d/%04d %02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    return buffer;
}

}  // namespace

LibraryWindow::LibraryWindow(HINSTANCE instance, HWND owner, std::function<Folders()> folders)
    : instance_(instance), owner_(owner), folders_(std::move(folders)) {
    backgroundBrush_ = CreateSolidBrush(theme::kBackground);
    surfaceBrush_ = CreateSolidBrush(theme::kSurface);
}

LibraryWindow::~LibraryWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
    if (buttonFont_) DeleteObject(buttonFont_);
    if (headerFont_) DeleteObject(headerFont_);
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
    if (surfaceBrush_) DeleteObject(surfaceBrush_);
}

void LibraryWindow::open() {
    if (!hwnd_ && !create()) return;
    refresh();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

bool LibraryWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &LibraryWindow::wndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = backgroundBrush_;
    wc.hIcon = theme::appIcon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = theme::appIcon(GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    RECT frame{0, 0, kClientWidth, kClientHeight};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
    AdjustWindowRect(&frame, style, FALSE);
    // WS_EX_APPWINDOW: a janela dona (principal) fica escondida; sem isso esta não apareceria na barra de tarefas.
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"Biblioteca do Qlip", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            frame.right - frame.left, frame.bottom - frame.top, owner_, nullptr, instance_, this);
    if (!hwnd_) {
        QLOG_ERROR(L"Falha ao criar a biblioteca.");
        return false;
    }
    theme::applyDarkTitleBar(hwnd_);

    font_ = theme::createFont(13);
    buttonFont_ = theme::createFont(14, FW_SEMIBOLD);
    headerFont_ = theme::createFont(12, FW_SEMIBOLD);

    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kIdList), instance_, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    // Tema escuro nativo na lista e no cabeçalho; cores próprias no corpo.
    SetWindowTheme(list_, L"", L"");
    // O cabeçalho é outra janela e não herda o tema: precisa do tema escuro próprio de cabeçalho.
    SetWindowTheme(ListView_GetHeader(list_), L"DarkMode_ItemsView", nullptr);
    ListView_SetBkColor(list_, theme::kSurface);
    ListView_SetTextBkColor(list_, theme::kSurface);
    ListView_SetTextColor(list_, theme::kText);
    // O cabeçalho manda o NM_CUSTOMDRAW para a própria lista: a subclasse intercepta e desenha no tema.
    SetWindowSubclass(list_, &LibraryWindow::listSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    const struct { const wchar_t* title; int width; } columns[] = {
        {L"Nome", 280}, {L"Tipo", 80}, {L"Duração", 90}, {L"Tamanho", 90}, {L"Data", 130},
    };
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[i].title);
        column.cx = columns[i].width;
        column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }

    countLabel_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 0, 300, 20, hwnd_, nullptr, instance_, nullptr);
    openButton_ = CreateWindowExW(0, L"BUTTON", L"Abrir", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                  reinterpret_cast<HMENU>(kIdOpen), instance_, nullptr);
    folderButton_ = CreateWindowExW(0, L"BUTTON", L"Mostrar na pasta", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                    hwnd_, reinterpret_cast<HMENU>(kIdFolder), instance_, nullptr);
    deleteButton_ = CreateWindowExW(0, L"BUTTON", L"Excluir", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                    reinterpret_cast<HMENU>(kIdDelete), instance_, nullptr);
    refreshButton_ = CreateWindowExW(0, L"BUTTON", L"Atualizar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                     reinterpret_cast<HMENU>(kIdRefresh), instance_, nullptr);

    for (HWND control : {countLabel_, list_}) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    layout();
    return true;
}

LRESULT CALLBACK LibraryWindow::listSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<LibraryWindow*>(data);
    if (msg == WM_NOTIFY) {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->code == NM_CUSTOMDRAW && header->hwndFrom == ListView_GetHeader(hwnd)) {
            return self->drawHeader(*reinterpret_cast<NMCUSTOMDRAW*>(lParam));
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, &LibraryWindow::listSubclassProc, 1);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT LibraryWindow::drawHeader(NMCUSTOMDRAW& draw) const {
    switch (draw.dwDrawStage) {
        case CDDS_PREPAINT:
            // Fundo inteiro do cabeçalho (inclui a sobra à direita da última coluna).
            FillRect(draw.hdc, &draw.rc, surfaceBrush_);
            return CDRF_NOTIFYITEMDRAW;

        case CDDS_ITEMPREPAINT: {
            // Cada coluna: mesmo fundo, texto discreto e sem divisórias.
            FillRect(draw.hdc, &draw.rc, surfaceBrush_);
            wchar_t text[64]{};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = text;
            item.cchTextMax = 64;
            Header_GetItem(draw.hdr.hwndFrom, static_cast<int>(draw.dwItemSpec), &item);
            RECT textRect = draw.rc;
            textRect.left += 8;
            SetBkMode(draw.hdc, TRANSPARENT);
            SetTextColor(draw.hdc, theme::kTextMuted);
            HFONT previous = static_cast<HFONT>(SelectObject(draw.hdc, headerFont_));
            DrawTextW(draw.hdc, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(draw.hdc, previous);
            return CDRF_SKIPDEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

void LibraryWindow::layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = 16;
    const int buttonsY = height - margin - 36;

    MoveWindow(list_, margin, 40, width - 2 * margin, buttonsY - 56, TRUE);
    MoveWindow(countLabel_, margin, 14, 400, 20, TRUE);

    // Botões alinhados à direita, Abrir primeiro (ação principal).
    int x = width - margin;
    for (HWND button : {refreshButton_, deleteButton_, folderButton_, openButton_}) {
        const int buttonWidth = button == folderButton_ ? 150 : 110;
        x -= buttonWidth;
        MoveWindow(button, x, buttonsY, buttonWidth, 36, TRUE);
        x -= 10;
    }
}

void LibraryWindow::refresh() {
    items_.clear();
    const Folders folders = folders_();

    auto collect = [this](const std::filesystem::path& folder, bool clip) {
        std::error_code ec;
        if (folder.empty() || !std::filesystem::exists(folder, ec)) return;
        // Iteração com error_code: um erro de leitura não pode lançar exceção dentro do procedimento Win32.
        std::filesystem::directory_iterator it(folder, ec);
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) continue;
            const auto path = entry.path();
            // Só MP4 finalizados: arquivos temporários (.tmp, .recording.mkv) ficam de fora.
            if (path.extension() != L".mp4") continue;
            Item item;
            item.path = path;
            item.clip = clip;
            item.bytes = entry.file_size(ec);
            const auto time = std::filesystem::last_write_time(path, ec);
            item.modifiedFileTime = static_cast<int64_t>(time.time_since_epoch().count());
            items_.push_back(item);
        }
    };
    collect(folders.recordings, false);
    collect(folders.clips, true);

    // Mais recentes primeiro; a duração só é lida agora (lê o cabeçalho de cada arquivo).
    std::sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.modifiedFileTime > b.modifiedFileTime; });
    for (Item& item : items_) item.durationSeconds = mediaDurationSeconds(item.path);
    fillList();
}

void LibraryWindow::fillList() {
    ListView_DeleteAllItems(list_);
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& item = items_[i];

        LVITEMW row{};
        row.mask = LVIF_TEXT | LVIF_PARAM;
        row.iItem = static_cast<int>(i);
        row.lParam = static_cast<LPARAM>(i);
        const std::wstring name = item.path.filename().wstring();
        row.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(list_, &row);

        const std::wstring type = item.clip ? L"Clipe" : L"Gravação";
        const std::wstring duration = formatDuration(item.durationSeconds);
        const std::wstring size = formatSize(item.bytes);
        const std::wstring date = formatDate(item.modifiedFileTime);
        const std::wstring* columns[] = {&type, &duration, &size, &date};
        for (int sub = 0; sub < 4; ++sub) {
            ListView_SetItemText(list_, static_cast<int>(i), sub + 1, const_cast<wchar_t*>(columns[sub]->c_str()));
        }
    }

    const std::wstring count = std::to_wstring(items_.size()) + (items_.size() == 1 ? L" arquivo" : L" arquivos");
    SetWindowTextW(countLabel_, count.c_str());
}

int LibraryWindow::selectedIndex() const {
    const int index = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (index < 0) return -1;
    LVITEMW row{};
    row.mask = LVIF_PARAM;
    row.iItem = index;
    ListView_GetItem(list_, &row);
    return static_cast<int>(row.lParam);
}

void LibraryWindow::openSelected() {
    const int index = selectedIndex();
    if (index < 0) return;
    ShellExecuteW(hwnd_, L"open", items_[index].path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void LibraryWindow::showSelectedInFolder() {
    const int index = selectedIndex();
    if (index < 0) return;
    const std::wstring arguments = L"/select,\"" + items_[index].path.wstring() + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

void LibraryWindow::deleteSelected() {
    const int index = selectedIndex();
    if (index < 0) return;
    const std::filesystem::path path = items_[index].path;
    // Sem caminho absoluto a lixeira não é usada e o arquivo sairia de vez: recusa.
    if (!path.is_absolute()) {
        MessageBoxW(hwnd_, L"Caminho inválido: escolha uma pasta absoluta nas configurações.", L"Qlip", MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring question = L"Mover \"" + path.filename().wstring() + L"\" para a lixeira?";
    if (MessageBoxW(hwnd_, question.c_str(), L"Excluir", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    // Para a lixeira (não apaga de vez): o arquivo pode ser recuperado pelo Windows.
    std::wstring from = path.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = hwnd_;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    // WANTNUKEWARNING: se não houver lixeira neste volume, o Windows pede confirmação antes de apagar de vez.
    op.fFlags = FOF_ALLOWUNDO | FOF_WANTNUKEWARNING | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
        MessageBoxW(hwnd_, L"Não foi possível mover o arquivo para a lixeira.", L"Qlip", MB_OK | MB_ICONWARNING);
        return;
    }
    refresh();
}

void LibraryWindow::drawButton(const DRAWITEMSTRUCT& item) const {
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = item.hwndItem == openButton_;
    RECT rect = item.rcItem;

    FillRect(item.hDC, &rect, backgroundBrush_);
    COLORREF fill = primary ? (pressed ? theme::kAccentPressed : theme::kAccent)
                            : (pressed ? theme::kSurfaceHover : kInput);
    theme::fillRoundRect(item.hDC, rect, 10, fill, primary ? CLR_NONE : theme::kBorder);

    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, 64);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, theme::kText);
    HFONT previous = static_cast<HFONT>(SelectObject(item.hDC, buttonFont_));
    DrawTextW(item.hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, previous);
}

LRESULT CALLBACK LibraryWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LibraryWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<LibraryWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<LibraryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT LibraryWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            layout();
            return 0;

        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, backgroundBrush_);
            return 1;
        }

        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wParam), theme::kBackground);
            SetTextColor(reinterpret_cast<HDC>(wParam), theme::kTextMuted);
            return reinterpret_cast<LRESULT>(backgroundBrush_);

        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON) {
                drawButton(*item);
                return TRUE;
            }
            break;
        }

        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->idFrom == kIdList && header->code == NM_DBLCLK) openSelected();
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kIdOpen: openSelected(); return 0;
                case kIdFolder: showSelectedInFolder(); return 0;
                case kIdDelete: deleteSelected(); return 0;
                case kIdRefresh: refresh(); return 0;
            }
            break;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
