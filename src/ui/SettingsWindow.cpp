#include "ui/SettingsWindow.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <cwchar>
#include <iterator>
#include <string>

#include "audio/AudioDevices.h"
#include "capture/Monitors.h"
#include "ui/Dropdown.h"
#include "ui/Theme.h"
#include "utils/Logger.h"

namespace {

constexpr wchar_t kWindowClass[] = L"QlipSettingsWindow";
constexpr UINT_PTR kIdRecordingsBrowse = 2001;
constexpr UINT_PTR kIdClipsBrowse = 2002;
constexpr UINT_PTR kIdSave = 2003;
constexpr UINT_PTR kIdCancel = 2004;
constexpr UINT_PTR kIdGameAudio = 2010;
constexpr UINT_PTR kIdMicAudio = 2011;
constexpr UINT_PTR kIdStartup = 2012;
constexpr UINT_PTR kIdHotkeyBase = 2020;  // 2020, 2021, 2022
constexpr UINT_PTR kIdValueLabel = 3001;  // valor dos volumes: texto claro; os demais rótulos são discretos

constexpr COLORREF kInput = theme::kInput;

// Layout em pixels. Duas colunas de cards, atalhos em faixa horizontal e rodapé.
constexpr int kClientWidth = 656;
constexpr int kClientHeight = 568;
constexpr int kRowHeight = 36;
constexpr int kPad = 16;
constexpr RECT kFoldersCard{20, 20, 320, 172};
constexpr RECT kRecordCard{20, 184, 320, 408};
constexpr RECT kReplayCard{336, 20, 636, 186};
constexpr RECT kAudioCard{336, 198, 636, 410};
constexpr RECT kHotkeysCard{20, 422, 636, 504};
constexpr RECT kStartupRect{20, 520, 280, 548};
constexpr RECT kSaveRect{476, 516, 636, 552};
constexpr RECT kCancelRect{326, 516, 466, 552};

RECT inset(const RECT& card, int x, int y, int width, int height) {
    return RECT{card.left + x, card.top + y, card.left + x + width, card.top + y + height};
}

HWND makeLabel(HWND parent, HINSTANCE instance, const wchar_t* text, const RECT& rect, UINT_PTR id = 0) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, rect.left, rect.top, rect.right - rect.left,
                           rect.bottom - rect.top, parent, reinterpret_cast<HMENU>(id), instance, nullptr);
}

HWND makeOwnerButton(HWND parent, HINSTANCE instance, const wchar_t* text, const RECT& rect, UINT_PTR id) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                           rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, parent,
                           reinterpret_cast<HMENU>(id), instance, nullptr);
}

bool isModifierKey(UINT vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN;
}

}  // namespace

std::wstring SettingsWindow::keyName(unsigned hotkey) {
    const UINT virtualKey = hotkeyVk(hotkey);
    if (virtualKey == 0) return L"Nenhuma";
    std::wstring prefix;
    const unsigned mods = hotkeyMods(hotkey);
    if (mods & MOD_CONTROL) prefix += L"Ctrl + ";
    if (mods & MOD_ALT) prefix += L"Alt + ";
    if (mods & MOD_SHIFT) prefix += L"Shift + ";
    // Teclas estendidas (setas, Insert, Delete...) precisam do bit de extensão no lParam.
    const bool extended = virtualKey == VK_INSERT || virtualKey == VK_DELETE || virtualKey == VK_HOME ||
                          virtualKey == VK_END || virtualKey == VK_PRIOR || virtualKey == VK_NEXT ||
                          virtualKey == VK_LEFT || virtualKey == VK_RIGHT || virtualKey == VK_UP || virtualKey == VK_DOWN;
    const UINT scan = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scan) << 16;
    if (extended) lParam |= 1L << 24;
    wchar_t name[64]{};
    if (GetKeyNameTextW(lParam, name, 64) == 0) return prefix + L"Tecla " + std::to_wstring(virtualKey);
    return prefix + name;
}

SettingsWindow::SettingsWindow(HINSTANCE instance, HWND owner, Callbacks callbacks)
    : instance_(instance), owner_(owner), callbacks_(std::move(callbacks)) {
    backgroundBrush_ = CreateSolidBrush(theme::kBackground);
    cardBrush_ = CreateSolidBrush(theme::kSurface);
    inputBrush_ = CreateSolidBrush(kInput);
}

SettingsWindow::~SettingsWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    for (HFONT font : {labelFont_, titleFont_, buttonFont_}) {
        if (font) DeleteObject(font);
    }
    for (HBRUSH brush : {backgroundBrush_, cardBrush_, inputBrush_}) {
        if (brush) DeleteObject(brush);
    }
}

void SettingsWindow::open(const Settings& current) {
    if (!hwnd_ && !create()) return;
    loadValues(current);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

bool SettingsWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SettingsWindow::wndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = backgroundBrush_;
    wc.hIcon = theme::appIcon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = theme::appIcon(GetSystemMetrics(SM_CXSMICON));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);  // falha silenciosa se já registrada

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    RECT frame{0, 0, kClientWidth, kClientHeight};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&frame, style, FALSE);

    hwnd_ = CreateWindowExW(0, kWindowClass, L"Configurações do Qlip", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            frame.right - frame.left, frame.bottom - frame.top, owner_, nullptr, instance_, this);
    if (!hwnd_) {
        QLOG_ERROR(L"Falha ao criar a janela de configurações.");
        return false;
    }
    theme::applyDarkTitleBar(hwnd_);

    labelFont_ = theme::createFont(13);
    titleFont_ = theme::createFont(12, FW_SEMIBOLD);
    buttonFont_ = theme::createFont(14, FW_SEMIBOLD);
    buildControls(hwnd_);
    return true;
}

HWND SettingsWindow::makeSelector(HWND parent, const RECT& rect) {
    HWND selector = makeOwnerButton(parent, instance_, L"", rect, 0);
    selectors_[selector] = Selector{};
    return selector;
}

void SettingsWindow::addOption(HWND selector, const wchar_t* text, int value) {
    selectors_[selector].options.emplace_back(text, value);
    InvalidateRect(selector, nullptr, FALSE);
}

void SettingsWindow::setSelected(HWND selector, int value) {
    Selector& state = selectors_[selector];
    state.index = 0;
    for (size_t i = 0; i < state.options.size(); ++i) {
        if (state.options[i].second == value) {
            state.index = static_cast<int>(i);
            break;
        }
    }
    InvalidateRect(selector, nullptr, FALSE);
}

int SettingsWindow::selectedValueOf(HWND selector) const {
    const auto found = selectors_.find(selector);
    if (found == selectors_.end() || found->second.options.empty()) return 0;
    return found->second.options[found->second.index].second;
}

void SettingsWindow::openSelector(HWND selector) {
    Selector& state = selectors_[selector];
    if (state.options.empty()) return;
    std::vector<std::wstring> labels;
    labels.reserve(state.options.size());
    for (const auto& option : state.options) labels.push_back(option.first);

    RECT anchor{};
    GetWindowRect(selector, &anchor);
    const int chosen = showDropdown(hwnd_, anchor, labels, state.index, labelFont_);
    if (chosen >= 0) state.index = chosen;
    InvalidateRect(selector, nullptr, FALSE);
}

void SettingsWindow::buildControls(HWND parent) {
    // Card "Pastas": rótulo, campo somente leitura e botão de procurar, para cada pasta.
    makeLabel(parent, instance_, L"PASTAS", inset(kFoldersCard, kPad, 12, 200, 18));
    makeLabel(parent, instance_, L"Gravações", inset(kFoldersCard, kPad, 38, 200, 18));
    recordingsEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                      kFoldersCard.left + kPad, kFoldersCard.top + 58, 206, 26, parent, nullptr, instance_, nullptr);
    makeOwnerButton(parent, instance_, L"…", inset(kFoldersCard, 228, 58, 56, 26), kIdRecordingsBrowse);
    makeLabel(parent, instance_, L"Clipes", inset(kFoldersCard, kPad, 94, 200, 18));
    clipsEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                 kFoldersCard.left + kPad, kFoldersCard.top + 114, 206, 26, parent, nullptr, instance_, nullptr);
    makeOwnerButton(parent, instance_, L"…", inset(kFoldersCard, 228, 114, 56, 26), kIdClipsBrowse);

    // Seletores: rótulo à esquerda, valor clicável à direita.
    auto selectorRow = [&](const RECT& card, int row, const wchar_t* label) {
        makeLabel(parent, instance_, label, inset(card, kPad, 38 + row * kRowHeight, 96, 26));
        return makeSelector(parent, inset(card, 116, 36 + row * kRowHeight, 168, 26));
    };

    // Card "Gravação".
    makeLabel(parent, instance_, L"GRAVAÇÃO", inset(kRecordCard, kPad, 12, 240, 18));
    targetSelector_ = selectorRow(kRecordCard, 0, L"O que gravar");
    addOption(targetSelector_, L"Tela inteira", 0);
    addOption(targetSelector_, L"Só o jogo", 1);

    resolutionSelector_ = selectorRow(kRecordCard, 1, L"Resolução");
    addOption(resolutionSelector_, L"Original", 0);
    addOption(resolutionSelector_, L"1080p", 1080);
    addOption(resolutionSelector_, L"720p", 720);

    recordFpsSelector_ = selectorRow(kRecordCard, 2, L"FPS");
    addOption(recordFpsSelector_, L"30 FPS", 30);
    addOption(recordFpsSelector_, L"60 FPS", 60);
    addOption(recordFpsSelector_, L"120 FPS", 120);

    recordQualitySelector_ = selectorRow(kRecordCard, 3, L"Qualidade");
    addOption(recordQualitySelector_, L"Baixa · 6 Mbps", 0);
    addOption(recordQualitySelector_, L"Média · 12 Mbps", 1);
    addOption(recordQualitySelector_, L"Alta · 25 Mbps", 2);
    addOption(recordQualitySelector_, L"Máxima · 40 Mbps", 3);

    // Monitor: as opções são montadas ao abrir a janela (monitores podem ser conectados depois).
    monitorSelector_ = selectorRow(kRecordCard, 4, L"Monitor");

    // Card "Clipe de replay".
    makeLabel(parent, instance_, L"CLIPE DE REPLAY", inset(kReplayCard, kPad, 12, 240, 18));
    replaySelector_ = selectorRow(kReplayCard, 0, L"Duração");
    addOption(replaySelector_, L"15 segundos", 15);
    addOption(replaySelector_, L"30 segundos", 30);
    addOption(replaySelector_, L"60 segundos", 60);
    addOption(replaySelector_, L"120 segundos", 120);

    clipFpsSelector_ = selectorRow(kReplayCard, 1, L"FPS");
    addOption(clipFpsSelector_, L"30 FPS", 30);
    addOption(clipFpsSelector_, L"60 FPS", 60);
    addOption(clipFpsSelector_, L"120 FPS", 120);

    clipQualitySelector_ = selectorRow(kReplayCard, 2, L"Qualidade");
    addOption(clipQualitySelector_, L"Baixa · 6 Mbps", 0);
    addOption(clipQualitySelector_, L"Média · 12 Mbps", 1);
    addOption(clipQualitySelector_, L"Alta · 25 Mbps", 2);
    addOption(clipQualitySelector_, L"Máxima · 40 Mbps", 3);

    // Card "Áudio": cada fonte tem liga/desliga e volume.
    makeLabel(parent, instance_, L"ÁUDIO", inset(kAudioCard, kPad, 12, 240, 18));
    gameAudioCheck_ = makeOwnerButton(parent, instance_, L"Som do jogo (sistema)", inset(kAudioCard, kPad, 38, 268, 24), kIdGameAudio);
    gameVolume_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                  kAudioCard.left + kPad, kAudioCard.top + 70, 172, 26, parent, nullptr, instance_, nullptr);
    SendMessageW(gameVolume_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    gameVolumeLabel_ = makeLabel(parent, instance_, L"100%", inset(kAudioCard, 196, 74, 80, 18), kIdValueLabel);

    micAudioCheck_ = makeOwnerButton(parent, instance_, L"Gravar o microfone", inset(kAudioCard, kPad, 104, 268, 24), kIdMicAudio);
    micVolume_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                 kAudioCard.left + kPad, kAudioCard.top + 136, 172, 26, parent, nullptr, instance_, nullptr);
    SendMessageW(micVolume_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    micVolumeLabel_ = makeLabel(parent, instance_, L"100%", inset(kAudioCard, 196, 140, 80, 18), kIdValueLabel);

    // Dispositivo do microfone: opções montadas ao abrir a janela.
    makeLabel(parent, instance_, L"Dispositivo", inset(kAudioCard, kPad, 172, 96, 26));
    micDeviceSelector_ = makeSelector(parent, inset(kAudioCard, 116, 170, 168, 26));

    // Card "Atalhos": um botão por ação. Clicar captura a próxima tecla pressionada.
    makeLabel(parent, instance_, L"ATALHOS  ·  clique e pressione a tecla, com Ctrl/Alt/Shift se quiser (Esc cancela)",
              inset(kHotkeysCard, kPad, 10, 560, 18));
    const wchar_t* hotkeyLabels[kHotkeyCount] = {L"Gravar / parar", L"Ligar o replay", L"Salvar clipe", L"Painel no jogo"};
    constexpr int kHotkeyWidth = 137;  // 4 colunas com 12 px entre elas
    for (int i = 0; i < kHotkeyCount; ++i) {
        const int x = kPad + i * (kHotkeyWidth + 12);
        makeLabel(parent, instance_, hotkeyLabels[i], inset(kHotkeysCard, x, 32, kHotkeyWidth, 18));
        hotkeyButtons_[i] = makeOwnerButton(parent, instance_, L"", inset(kHotkeysCard, x, 50, kHotkeyWidth, 26), kIdHotkeyBase + i);
    }

    // Inicialização com o Windows e botões de ação.
    startupCheck_ = makeOwnerButton(parent, instance_, L"Iniciar com o Windows", kStartupRect, kIdStartup);
    makeOwnerButton(parent, instance_, L"Cancelar", kCancelRect, kIdCancel);
    makeOwnerButton(parent, instance_, L"Salvar", kSaveRect, kIdSave);

    // Fonte padrão e tema escuro nas trilhas de volume (controle nativo).
    EnumChildWindows(parent, [](HWND child, LPARAM font) {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(labelFont_));
    for (HWND track : {gameVolume_, micVolume_}) {
        SetWindowTheme(track, L"DarkMode_Explorer", nullptr);
    }
}

void SettingsWindow::loadValues(const Settings& settings) {
    // Se a janela foi reaberta durante a captura de uma tecla, encerra a captura antes (restaura os atalhos globais).
    endKeyCapture(false, 0);
    base_ = settings;
    SetWindowTextW(recordingsEdit_, settings.recordingsFolder.empty() ? L"Padrão: Vídeos\\Qlip" : settings.recordingsFolder.c_str());
    SetWindowTextW(clipsEdit_, settings.clipsFolder.empty() ? L"Padrão: Vídeos\\Qlip\\Clipes" : settings.clipsFolder.c_str());
    setSelected(targetSelector_, settings.captureWindow ? 1 : 0);
    setSelected(resolutionSelector_, settings.resolutionHeight);
    setSelected(recordFpsSelector_, settings.recordFps);
    setSelected(recordQualitySelector_, settings.recordQuality);
    setSelected(replaySelector_, settings.replaySeconds);
    setSelected(clipFpsSelector_, settings.clipFps);
    setSelected(clipQualitySelector_, settings.clipQuality);
    rebuildDeviceSelectors(settings);
    setCheck(gameAudioCheck_, settings.recordGameAudio);
    setCheck(micAudioCheck_, settings.recordMicAudio);
    setCheck(startupCheck_, settings.startWithWindows);
    SendMessageW(gameVolume_, TBM_SETPOS, TRUE, settings.gameVolume);
    SendMessageW(micVolume_, TBM_SETPOS, TRUE, settings.micVolume);
    updateVolumeLabel(gameVolume_, gameVolumeLabel_);
    updateVolumeLabel(micVolume_, micVolumeLabel_);

    hotkeys_[0] = settings.hotkeyRecord;
    hotkeys_[1] = settings.hotkeyReplay;
    hotkeys_[2] = settings.hotkeySave;
    hotkeys_[3] = settings.hotkeyOverlay;
    capturing_ = -1;
    for (int i = 0; i < kHotkeyCount; ++i) SetWindowTextW(hotkeyButtons_[i], keyName(hotkeys_[i]).c_str());
}

void SettingsWindow::rebuildDeviceSelectors(const Settings& settings) {
    // Monitores: "Principal" (vazio) + cada monitor conectado. Um monitor salvo que não está conectado
    // continua como opção, para não perder a escolha quando ele for religado.
    Selector& monitors = selectors_[monitorSelector_];
    monitors.options.clear();
    monitorDevices_ = {L""};
    monitors.options.emplace_back(L"Principal", 0);
    int monitorSelected = 0;
    for (const MonitorEntry& entry : listMonitors()) {
        monitorDevices_.push_back(entry.deviceName);
        monitors.options.emplace_back(entry.label, static_cast<int>(monitorDevices_.size() - 1));
        if (entry.deviceName == settings.monitorDevice) monitorSelected = static_cast<int>(monitorDevices_.size() - 1);
    }
    if (!settings.monitorDevice.empty() && monitorSelected == 0) {
        monitorDevices_.push_back(settings.monitorDevice);
        monitors.options.emplace_back(L"Desconectado: " + settings.monitorDevice, static_cast<int>(monitorDevices_.size() - 1));
        monitorSelected = static_cast<int>(monitorDevices_.size() - 1);
    }
    setSelected(monitorSelector_, monitorSelected);

    // Microfones: "Padrão do Windows" (vazio) + cada dispositivo de captura ativo.
    Selector& mics = selectors_[micDeviceSelector_];
    mics.options.clear();
    micDeviceIds_ = {L""};
    mics.options.emplace_back(L"Padrão do Windows", 0);
    int micSelected = 0;
    for (const AudioDeviceEntry& device : listCaptureDevices()) {
        micDeviceIds_.push_back(device.id);
        mics.options.emplace_back(device.name, static_cast<int>(micDeviceIds_.size() - 1));
        if (device.id == settings.micDeviceId) micSelected = static_cast<int>(micDeviceIds_.size() - 1);
    }
    if (!settings.micDeviceId.empty() && micSelected == 0) {
        micDeviceIds_.push_back(settings.micDeviceId);
        mics.options.emplace_back(L"Microfone desconectado", static_cast<int>(micDeviceIds_.size() - 1));
        micSelected = static_cast<int>(micDeviceIds_.size() - 1);
    }
    setSelected(micDeviceSelector_, micSelected);
}

Settings SettingsWindow::readValues() const {
    Settings settings = base_;  // mantém opções que não aparecem aqui
    auto text = [](HWND edit) {
        wchar_t buffer[MAX_PATH * 2]{};
        GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
        return std::wstring(buffer);
    };
    // Textos "Padrão: ..." são só exibição: significam pasta vazia nas configurações.
    const std::wstring recording = text(recordingsEdit_);
    const std::wstring clips = text(clipsEdit_);
    settings.recordingsFolder = recording.rfind(L"Padrão:", 0) == 0 ? L"" : recording;
    settings.clipsFolder = clips.rfind(L"Padrão:", 0) == 0 ? L"" : clips;

    settings.captureWindow = selectedValueOf(targetSelector_) == 1;
    settings.resolutionHeight = selectedValueOf(resolutionSelector_);
    settings.recordFps = selectedValueOf(recordFpsSelector_);
    settings.recordQuality = selectedValueOf(recordQualitySelector_);
    settings.replaySeconds = selectedValueOf(replaySelector_);
    settings.clipFps = selectedValueOf(clipFpsSelector_);
    settings.clipQuality = selectedValueOf(clipQualitySelector_);
    const int monitorIndex = selectedValueOf(monitorSelector_);
    settings.monitorDevice = monitorIndex >= 0 && monitorIndex < static_cast<int>(monitorDevices_.size()) ? monitorDevices_[monitorIndex] : L"";
    const int micIndex = selectedValueOf(micDeviceSelector_);
    settings.micDeviceId = micIndex >= 0 && micIndex < static_cast<int>(micDeviceIds_.size()) ? micDeviceIds_[micIndex] : L"";
    settings.recordGameAudio = isChecked(gameAudioCheck_);
    settings.recordMicAudio = isChecked(micAudioCheck_);
    settings.startWithWindows = isChecked(startupCheck_);
    settings.gameVolume = static_cast<int>(SendMessageW(gameVolume_, TBM_GETPOS, 0, 0));
    settings.micVolume = static_cast<int>(SendMessageW(micVolume_, TBM_GETPOS, 0, 0));
    settings.hotkeyRecord = hotkeys_[0];
    settings.hotkeyReplay = hotkeys_[1];
    settings.hotkeySave = hotkeys_[2];
    settings.hotkeyOverlay = hotkeys_[3];
    return settings;
}

bool SettingsWindow::validateHotkeys() const {
    for (int i = 0; i < kHotkeyCount; ++i) {
        for (int j = i + 1; j < kHotkeyCount; ++j) {
            if (hotkeys_[i] == hotkeys_[j]) return false;
        }
    }
    return true;
}

void SettingsWindow::beginKeyCapture(int index) {
    if (capturing_ >= 0) endKeyCapture(true, hotkeys_[capturing_]);
    capturing_ = index;
    SetWindowTextW(hotkeyButtons_[index], L"Pressione uma tecla…");
    if (callbacks_.onCapturing) callbacks_.onCapturing(true);  // senão o atalho global consumiria a tecla
    SetFocus(hwnd_);  // teclas vão para a janela, não para o botão
    InvalidateRect(hotkeyButtons_[index], nullptr, FALSE);
}

void SettingsWindow::endKeyCapture(bool keepKey, unsigned hotkey) {
    if (capturing_ < 0) return;
    const int index = capturing_;
    capturing_ = -1;
    if (keepKey) hotkeys_[index] = hotkey;
    SetWindowTextW(hotkeyButtons_[index], keyName(hotkeys_[index]).c_str());
    InvalidateRect(hotkeyButtons_[index], nullptr, FALSE);
    if (callbacks_.onCapturing) callbacks_.onCapturing(false);
}

void SettingsWindow::browseFolder(HWND edit) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SetWindowTextW(edit, path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

void SettingsWindow::updateVolumeLabel(HWND trackbar, HWND label) const {
    const int percent = static_cast<int>(SendMessageW(trackbar, TBM_GETPOS, 0, 0));
    const std::wstring text = std::to_wstring(percent) + L"%";
    SetWindowTextW(label, text.c_str());
}

void SettingsWindow::toggleCheck(HWND checkbox) {
    setCheck(checkbox, !isChecked(checkbox));
}

void SettingsWindow::setCheck(HWND checkbox, bool checked) {
    checks_[checkbox] = checked;
    InvalidateRect(checkbox, nullptr, FALSE);
}

bool SettingsWindow::isChecked(HWND checkbox) const {
    const auto found = checks_.find(checkbox);
    return found != checks_.end() && found->second;
}

void SettingsWindow::drawBackground(HDC dc, const RECT& client) const {
    FillRect(dc, &client, backgroundBrush_);
    for (const RECT& card : {kFoldersCard, kRecordCard, kReplayCard, kAudioCard, kHotkeysCard}) {
        theme::fillRoundRect(dc, card, 12, theme::kSurface, theme::kBorder);
    }
}

void SettingsWindow::drawSelector(const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    FillRect(item.hDC, &rect, cardBrush_);
    theme::fillRoundRect(item.hDC, rect, 6, kInput, theme::kBorder);

    // Texto da opção atual.
    std::wstring text;
    const auto found = selectors_.find(item.hwndItem);
    if (found != selectors_.end() && !found->second.options.empty()) {
        text = found->second.options[found->second.index].first;
    }
    RECT textRect{rect.left + 12, rect.top, rect.right - 28, rect.bottom};
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, theme::kText);
    HFONT previous = static_cast<HFONT>(SelectObject(item.hDC, labelFont_));
    DrawTextW(item.hDC, text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(item.hDC, previous);

    // Indicador de que é clicável: seta para baixo.
    const int cx = rect.right - 14;
    const int cy = (rect.top + rect.bottom) / 2;
    POINT arrow[3] = {{cx - 5, cy - 2}, {cx + 5, cy - 2}, {cx, cy + 3}};
    HBRUSH brush = CreateSolidBrush(theme::kTextMuted);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
    Polygon(item.hDC, arrow, 3);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(brush);
}

void SettingsWindow::drawCheckbox(const DRAWITEMSTRUCT& item) const {
    RECT rect = item.rcItem;
    // A caixa de inicialização fica fora dos cards: usa o fundo da janela.
    FillRect(item.hDC, &rect, item.CtlID == kIdStartup ? backgroundBrush_ : cardBrush_);
    const bool checked = isChecked(item.hwndItem);

    const int size = 18;
    const int top = (rect.top + rect.bottom - size) / 2;
    RECT box{rect.left, top, rect.left + size, top + size};
    theme::fillRoundRect(item.hDC, box, 5, checked ? theme::kAccent : kInput, checked ? CLR_NONE : theme::kBorder);
    if (checked) {
        // Marca branca: duas linhas bastam neste tamanho.
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0xFF, 0xFF, 0xFF));
        HGDIOBJ oldPen = SelectObject(item.hDC, pen);
        MoveToEx(item.hDC, box.left + 4, box.top + 9, nullptr);
        LineTo(item.hDC, box.left + 8, box.top + 13);
        LineTo(item.hDC, box.left + 14, box.top + 5);
        SelectObject(item.hDC, oldPen);
        DeleteObject(pen);
    }

    wchar_t text[96]{};
    GetWindowTextW(item.hwndItem, text, 96);
    RECT textRect{rect.left + size + 10, rect.top, rect.right, rect.bottom};
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, theme::kText);
    HFONT previous = static_cast<HFONT>(SelectObject(item.hDC, labelFont_));
    DrawTextW(item.hDC, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, previous);
}

void SettingsWindow::drawButton(const DRAWITEMSTRUCT& item) const {
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = item.CtlID == kIdSave;
    const bool capturingThis = capturing_ >= 0 && item.CtlID == kIdHotkeyBase + capturing_;
    RECT rect = item.rcItem;

    // Fundo do card (ou da janela) por baixo, para o canto arredondado não deixar rastro.
    FillRect(item.hDC, &rect, cardBrush_);
    COLORREF fill = primary ? (pressed ? theme::kAccentPressed : theme::kAccent)
                            : (pressed || capturingThis ? theme::kSurfaceHover : kInput);
    COLORREF border = primary ? CLR_NONE : (capturingThis ? theme::kAccent : theme::kBorder);
    theme::fillRoundRect(item.hDC, rect, 10, fill, border);

    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, 64);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, theme::kText);
    HFONT previous = static_cast<HFONT>(SelectObject(item.hDC, buttonFont_));
    DrawTextW(item.hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, previous);
}

LRESULT CALLBACK SettingsWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            // Seletores não têm ID (0): o clique chega com o handle no lParam.
            if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == 0 && lParam && selectors_.count(reinterpret_cast<HWND>(lParam))) {
                openSelector(reinterpret_cast<HWND>(lParam));
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) >= kIdHotkeyBase && LOWORD(wParam) < kIdHotkeyBase + kHotkeyCount) {
                beginKeyCapture(static_cast<int>(LOWORD(wParam) - kIdHotkeyBase));
                return 0;
            }
            switch (LOWORD(wParam)) {
                case kIdRecordingsBrowse:
                    browseFolder(recordingsEdit_);
                    return 0;
                case kIdClipsBrowse:
                    browseFolder(clipsEdit_);
                    return 0;
                case kIdGameAudio:
                case kIdMicAudio:
                case kIdStartup:
                    if (HIWORD(wParam) == BN_CLICKED) toggleCheck(reinterpret_cast<HWND>(lParam));
                    return 0;
                case kIdCancel:
                    endKeyCapture(false, 0);
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                case kIdSave: {
                    if (!validateHotkeys()) {
                        MessageBoxW(hwnd, L"Cada ação precisa de uma tecla diferente.", L"Qlip", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    Settings settings = readValues();
                    SettingsManager manager;
                    manager.settings() = settings;
                    if (!manager.save()) {
                        MessageBoxW(hwnd, L"Não foi possível salvar as configurações.", L"Qlip", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    base_ = settings;
                    capturing_ = -1;
                    ShowWindow(hwnd, SW_HIDE);
                    if (callbacks_.onSaved) callbacks_.onSaved(settings);
                    return 0;
                }
            }
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // Captura de atalho: a próxima tecla vira o atalho (Esc cancela; modificadores são ignorados).
            if (capturing_ >= 0) {
                const UINT vk = static_cast<UINT>(wParam);
                if (vk == VK_ESCAPE) {
                    endKeyCapture(false, 0);
                } else if (!isModifierKey(vk)) {
                    // Modificadores pressionados junto com a tecla entram no atalho (ex.: Alt + X).
                    unsigned mods = 0;
                    if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
                    if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
                    if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
                    swallowChar_ = msg == WM_SYSKEYDOWN;
                    endKeyCapture(true, makeHotkey(vk, mods));
                }
                return 0;
            }
            break;

        case WM_SYSCHAR:
            if (swallowChar_) {
                swallowChar_ = false;
                return 0;
            }
            break;

        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == gameVolume_) updateVolumeLabel(gameVolume_, gameVolumeLabel_);
            if (reinterpret_cast<HWND>(lParam) == micVolume_) updateVolumeLabel(micVolume_, micVolumeLabel_);
            return 0;

        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            drawBackground(reinterpret_cast<HDC>(wParam), client);
            return 1;
        }

        case WM_CTLCOLORSTATIC: {
            // Rótulos ficam sobre o card: mesma cor de fundo, texto discreto (o valor do volume é claro).
            HDC dc = reinterpret_cast<HDC>(wParam);
            const HWND control = reinterpret_cast<HWND>(lParam);
            SetBkColor(dc, theme::kSurface);
            SetTextColor(dc, GetDlgCtrlID(control) == kIdValueLabel ? theme::kText : theme::kTextMuted);
            return reinterpret_cast<LRESULT>(cardBrush_);
        }

        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), theme::kText);
            SetBkColor(reinterpret_cast<HDC>(wParam), kInput);
            return reinterpret_cast<LRESULT>(inputBrush_);

        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON) {
                if (selectors_.count(item->hwndItem)) {
                    drawSelector(*item);
                } else if (item->CtlID == kIdGameAudio || item->CtlID == kIdMicAudio || item->CtlID == kIdStartup) {
                    drawCheckbox(*item);
                } else {
                    drawButton(*item);
                }
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            // Fechar sem salvar descarta as alterações: só esconde.
            endKeyCapture(false, 0);
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
