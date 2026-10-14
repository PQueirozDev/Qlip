#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

#include "config/SettingsManager.h"

// Janela de configurações em cards. Cada alteração é salva no settings.json e vale a partir da próxima gravação.
class SettingsWindow {
public:
    struct Callbacks {
        // Chamado depois de salvar, com os valores novos.
        std::function<void(const Settings&)> onSaved;
        // true quando a captura de tecla começa, false quando termina: os atalhos globais são soltos durante ela.
        std::function<void(bool capturing)> onCapturing;
    };

    SettingsWindow(HINSTANCE instance, HWND owner, Callbacks callbacks);
    ~SettingsWindow();

    // Mostra a janela com os valores atuais (cria na primeira vez).
    void open(const Settings& current);

    // Nome legível de um atalho salvo (tecla + modificadores), ex.: "Alt + X".
    static std::wstring keyName(unsigned hotkey);

private:
    // Seletor próprio (substitui o combo nativo, que não aceita o tema escuro).
    // Clicar abre a lista suspensa com todas as opções.
    struct Selector {
        std::vector<std::pair<std::wstring, int>> options;  // texto e valor
        int index = 0;
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool create();
    void buildControls(HWND parent);
    HWND makeSelector(HWND parent, const RECT& rect);
    void addOption(HWND selector, const wchar_t* text, int value);
    void setSelected(HWND selector, int value);
    int selectedValueOf(HWND selector) const;
    void openSelector(HWND selector);

    void loadValues(const Settings& settings);
    void rebuildDeviceSelectors(const Settings& settings);
    Settings readValues() const;
    bool validateHotkeys() const;
    void beginKeyCapture(int index);
    void endKeyCapture(bool keepKey, unsigned hotkey);
    void browseFolder(HWND edit);
    void updateVolumeLabel(HWND trackbar, HWND label) const;
    void toggleCheck(HWND checkbox);
    void setCheck(HWND checkbox, bool checked);
    bool isChecked(HWND checkbox) const;

    void drawBackground(HDC dc, const RECT& client) const;
    void drawSelector(const DRAWITEMSTRUCT& item);
    void drawCheckbox(const DRAWITEMSTRUCT& item) const;
    void drawButton(const DRAWITEMSTRUCT& item) const;

    HINSTANCE instance_;
    HWND owner_;
    Callbacks callbacks_;
    HWND hwnd_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    HFONT labelFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT buttonFont_ = nullptr;

    // Valores que não aparecem na janela são preservados.
    Settings base_;
    std::map<HWND, Selector> selectors_;
    // Estado dos checkboxes (botões desenhados pelo proprietário não guardam o check nativo de forma confiável).
    std::map<HWND, bool> checks_;

    // Atalhos (tecla + modificadores, como em Settings): 0 gravar, 1 replay, 2 salvar clipe, 3 overlay.
    // capturing_ = -1: nenhum em captura.
    static constexpr int kHotkeyCount = 4;
    HWND hotkeyButtons_[kHotkeyCount] = {};
    unsigned hotkeys_[kHotkeyCount] = {};
    int capturing_ = -1;
    // Depois de capturar Alt + tecla, o WM_SYSCHAR seguinte é descartado (senão o Windows emite um bipe).
    bool swallowChar_ = false;

    HWND recordingsEdit_ = nullptr;
    HWND clipsEdit_ = nullptr;
    HWND targetSelector_ = nullptr;
    HWND resolutionSelector_ = nullptr;
    HWND recordFpsSelector_ = nullptr;
    HWND recordQualitySelector_ = nullptr;
    HWND replaySelector_ = nullptr;
    HWND clipFpsSelector_ = nullptr;
    HWND clipQualitySelector_ = nullptr;
    HWND monitorSelector_ = nullptr;
    HWND micDeviceSelector_ = nullptr;
    // Valor de cada opção dos seletores de dispositivo = índice nestes vetores (vazio = padrão).
    std::vector<std::wstring> monitorDevices_;
    std::vector<std::wstring> micDeviceIds_;
    HWND gameAudioCheck_ = nullptr;
    HWND micAudioCheck_ = nullptr;
    HWND startupCheck_ = nullptr;
    HWND gameVolume_ = nullptr;
    HWND micVolume_ = nullptr;
    HWND gameVolumeLabel_ = nullptr;
    HWND micVolumeLabel_ = nullptr;
};
