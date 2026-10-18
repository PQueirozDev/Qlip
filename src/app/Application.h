#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>

#include "config/SettingsManager.h"
#include "recording/RecordingManager.h"
#include "replay/ReplayManager.h"
#include "ui/LibraryWindow.h"
#include "ui/MainWindow.h"
#include "ui/OverlayWindow.h"
#include "ui/ToastWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/TrayIcon.h"

// Ciclo de vida do app: instância única, configurações, janela, bandeja, atalhos, gravação e replay.
class Application {
public:
    explicit Application(HINSTANCE instance);
    ~Application();

    // Retorna o código de saída do processo.
    int run();

private:
    static constexpr int kHotkeyRecord = 1;
    static constexpr int kHotkeyReplay = 2;
    static constexpr int kHotkeySave = 3;
    static constexpr int kHotkeyOverlay = 4;

    bool acquireSingleInstance();
    void requestExit();

    void toggleRecording();
    void startRecording();
    void stopRecording();
    void toggleReplay();
    void saveClip();
    void setNotice(const std::wstring& text);
    void updateStatus();
    void handleRecordingEvent();
    void registerHotkeys();
    void unregisterHotkeys();
    void applySettings(const Settings& settings);
    RecordingManager::Options captureOptions() const;

    HINSTANCE instance_;
    HANDLE singleInstanceMutex_ = nullptr;
    SettingsManager settings_;

    // Gravação e replay usam threads próprias, que chamam a UI via PostMessage.
    std::unique_ptr<RecordingManager> recording_;
    std::unique_ptr<ReplayManager> replay_;
    std::mutex eventMutex_;
    std::wstring pendingFatal_;
    std::wstring pendingReplayFatal_;
    // Aviso temporário na barra de status (ex.: "Clipe salvo"), até noticeUntil_.
    std::wstring notice_;
    uint64_t noticeUntil_ = 0;
    uint64_t noticeSeq_ = 0;  // muda a cada aviso novo
    uint64_t toastSeq_ = 0;   // último aviso já mostrado no canto da tela (só a thread da UI usa)

    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<TrayIcon> trayIcon_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    std::unique_ptr<LibraryWindow> library_;
    std::unique_ptr<OverlayWindow> overlay_;
    std::unique_ptr<ToastWindow> toast_;
};
