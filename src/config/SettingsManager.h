#pragma once

#include <filesystem>
#include <string>

struct Settings {
    // Gravação contínua
    int recordFps = 60;            // 30, 60 ou 120
    int recordQuality = 1;         // índice em qualityBitrateKbps(): 0 baixa, 1 média, 2 alta, 3 máxima
    int resolutionHeight = 1080;   // 0 = original, 720 ou 1080 (vale para gravação e clipe)

    // Clipe (replay instantâneo)
    int replaySeconds = 30;        // 15, 30, 60 ou 120
    int clipFps = 60;              // 30, 60 ou 120
    int clipQuality = 1;           // mesmo índice de qualidade da gravação

    // Alvo da captura
    bool captureWindow = false;    // false = área de trabalho (monitor); true = janela/jogo
    std::wstring monitorDevice;    // vazio = monitor principal; senão nome do dispositivo (ex.: \\.\DISPLAY2)

    // Áudio
    bool recordGameAudio = true;   // áudio do sistema (loopback)
    bool recordMicAudio = false;   // microfone
    std::wstring micDeviceId;      // vazio = microfone padrão do Windows; senão ID do dispositivo WASAPI
    int gameVolume = 100;          // 0 a 200 (%)
    int micVolume = 100;           // 0 a 200 (%)

    // Pastas
    std::wstring recordingsFolder; // vazio = Vídeos\Qlip
    std::wstring clipsFolder;      // vazio = Vídeos\Qlip\Clipes

    // Atalhos: virtual-key code nos 8 bits baixos e modificadores (MOD_ALT/CONTROL/SHIFT) a partir do bit 16.
    unsigned hotkeySave = 0x77;    // F8
    unsigned hotkeyRecord = 0x78;  // F9
    unsigned hotkeyReplay = 0x79;  // F10
    unsigned hotkeyOverlay = 0x50 | (0x0001u << 16);  // Alt + P (Alt + Z já é do overlay da NVIDIA)

    // Sistema
    bool startWithWindows = false;
};

// Partes de um atalho salvo em Settings.
constexpr unsigned hotkeyVk(unsigned hotkey) { return hotkey & 0xFF; }
constexpr unsigned hotkeyMods(unsigned hotkey) { return (hotkey >> 16) & 0x7; }
constexpr unsigned makeHotkey(unsigned vk, unsigned mods) { return (vk & 0xFF) | ((mods & 0x7) << 16); }

// Bitrate (kbps) de cada nível de qualidade. Único lugar com os valores.
int qualityBitrateKbps(int qualityIndex);

// Lê e grava settings.json em %APPDATA%\Qlip. Valores inválidos voltam ao padrão.
class SettingsManager {
public:
    static std::filesystem::path appDataDir();
    static std::filesystem::path settingsFile();
    // Move %APPDATA%\QClip (nome antigo do app) para %APPDATA%\Qlip, se ainda não houver a pasta nova.
    static void migrateLegacyAppData();

    bool load();
    bool save() const;

    Settings& settings() { return settings_; }
    const Settings& settings() const { return settings_; }

    // Pasta efetiva: a configurada, ou o padrão em Vídeos.
    std::filesystem::path recordingsFolder() const;
    std::filesystem::path clipsFolder() const;

private:
    Settings settings_;
};
