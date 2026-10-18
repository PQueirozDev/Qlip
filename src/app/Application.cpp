#include "app/Application.h"

#include <cstdio>

#include "config/Startup.h"
#include "ui/Theme.h"
#include "utils/Logger.h"
#include "utils/Text.h"

namespace {

constexpr wchar_t kSingleInstanceName[] = L"Local\\Qlip.SingleInstance";
constexpr uint64_t kNoticeMs = 4000;

std::wstring hhmmss(uint64_t totalSeconds) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02llu:%02llu:%02llu", static_cast<unsigned long long>(totalSeconds / 3600),
               static_cast<unsigned long long>((totalSeconds / 60) % 60), static_cast<unsigned long long>(totalSeconds % 60));
    return buffer;
}

}  // namespace

Application::Application(HINSTANCE instance) : instance_(instance) {}

Application::~Application() {
    // Gravação e replay devem estar encerrados antes de a janela sumir (os eventos usam a janela).
    if (recording_) {
        recording_->stop();
        recording_->waitForIdle();
    }
    if (replay_) {
        replay_->stop();
        replay_->waitForIdle();
    }
    recording_.reset();
    replay_.reset();
    overlay_.reset();
    toast_.reset();
    library_.reset();
    settingsWindow_.reset();
    trayIcon_.reset();
    mainWindow_.reset();
    if (singleInstanceMutex_) CloseHandle(singleInstanceMutex_);
    Logger::instance().stop();
}

bool Application::acquireSingleInstance() {
    singleInstanceMutex_ = CreateMutexW(nullptr, FALSE, kSingleInstanceName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Já existe uma instância: abrir o Qlip de novo (atalho, menu Iniciar) mostra o painel dela.
        if (HWND existing = FindWindowW(L"QlipMainWindow", nullptr)) {
            DWORD processId = 0;
            GetWindowThreadProcessId(existing, &processId);
            AllowSetForegroundWindow(processId);  // esta instância tem o foco; repassa para a outra abrir o painel
            PostMessageW(existing, RegisterWindowMessageW(MainWindow::kShowPanelMessageName), 0, 0);
        }
        return false;
    }
    return singleInstanceMutex_ != nullptr;
}

void Application::requestExit() {
    PostQuitMessage(0);
}

int Application::run() {
    SettingsManager::migrateLegacyAppData();  // antes do log: a pasta do log também é migrada
    Logger::instance().start(SettingsManager::appDataDir() / L"logs" / L"qlip.log");
    QLOG_INFO(L"Qlip iniciando.");

    if (!acquireSingleInstance()) {
        QLOG_INFO(L"Outra instância já está em execução; encerrando esta.");
        return 0;
    }

    if (!settings_.load()) {
        // Primeiro uso ou arquivo inválido: grava o padrão para o usuário poder editar.
        QLOG_INFO(L"Configurações ausentes ou inválidas; usando padrão.");
        settings_.save();
    }

    // Recupera gravações interrompidas por uma queda antes de começar a gravar de novo.
    RecordingManager::recoverLeftovers(settings_.recordingsFolder());

    RecordingManager::Callbacks recordingCallbacks;
    recordingCallbacks.onFinished = [this] {
        if (mainWindow_) mainWindow_->postRecordingEvent();
    };
    recordingCallbacks.onFatal = [this](const std::wstring& reason) {
        {
            std::lock_guard lock(eventMutex_);
            pendingFatal_ = reason;
        }
        if (mainWindow_) mainWindow_->postRecordingEvent();
    };
    recording_ = std::make_unique<RecordingManager>(std::move(recordingCallbacks));

    ReplayManager::Callbacks replayCallbacks;
    replayCallbacks.onClipSaved = [this](const std::filesystem::path& file) {
        setNotice(L"Clipe salvo: " + file.filename().wstring());
        if (mainWindow_) mainWindow_->postRecordingEvent();
    };
    replayCallbacks.onClipError = [this](const std::wstring& error) {
        setNotice(L"Erro ao salvar clipe: " + error);
        if (mainWindow_) mainWindow_->postRecordingEvent();
    };
    replayCallbacks.onFatal = [this](const std::wstring& reason) {
        {
            std::lock_guard lock(eventMutex_);
            pendingReplayFatal_ = reason;
        }
        if (mainWindow_) mainWindow_->postRecordingEvent();
    };
    replay_ = std::make_unique<ReplayManager>(std::move(replayCallbacks));

    MainWindow::Callbacks windowCallbacks;
    windowCallbacks.onTrayEvent = [this](LPARAM lParam) {
        if (trayIcon_) trayIcon_->handleMessage(lParam);
    };
    windowCallbacks.onTaskbarRecreated = [this] {
        // O Explorer reiniciou e removeu o ícone: recria para não perder o acesso ao app.
        if (trayIcon_ && mainWindow_) trayIcon_->create(mainWindow_->handle());
    };
    windowCallbacks.canHideToTray = [this] {
        return trayIcon_ && trayIcon_->isAdded();
    };
    windowCallbacks.onRecordClicked = [this] { toggleRecording(); };
    windowCallbacks.onSaveClipClicked = [this] { saveClip(); };
    windowCallbacks.onLibraryClicked = [this] {
        if (library_) library_->open();
    };
    windowCallbacks.onSettingsClicked = [this] {
        if (settingsWindow_) settingsWindow_->open(settings_.settings());
    };
    windowCallbacks.onHotkey = [this](int id) {
        if (id == kHotkeyRecord) toggleRecording();
        if (id == kHotkeyReplay) toggleReplay();
        if (id == kHotkeySave) saveClip();
        if (id == kHotkeyOverlay && overlay_) overlay_->toggle();
    };
    windowCallbacks.onTimer = [this] { updateStatus(); };
    windowCallbacks.onRecordingEvent = [this] { handleRecordingEvent(); };
    windowCallbacks.onShowPanelRequested = [this] {
        if (overlay_) overlay_->show();
    };
    windowCallbacks.onExitRequested = [this] {
        QLOG_INFO(L"Encerramento pedido pelo instalador.");
        requestExit();
    };
    mainWindow_ = std::make_unique<MainWindow>(std::move(windowCallbacks));
    if (!mainWindow_->create(instance_)) return 1;

    SettingsWindow::Callbacks settingsCallbacks;
    settingsCallbacks.onSaved = [this](const Settings& saved) { applySettings(saved); };
    // Durante a captura de uma tecla, os atalhos globais são soltos: senão a tecla dispararia a ação.
    settingsCallbacks.onCapturing = [this](bool capturing) {
        if (capturing) {
            unregisterHotkeys();
        } else {
            registerHotkeys();
        }
    };
    settingsWindow_ = std::make_unique<SettingsWindow>(instance_, mainWindow_->handle(), std::move(settingsCallbacks));

    library_ = std::make_unique<LibraryWindow>(instance_, mainWindow_->handle(), [this] {
        return LibraryWindow::Folders{settings_.recordingsFolder(), settings_.clipsFolder()};
    });

    OverlayWindow::Callbacks overlayCallbacks;
    overlayCallbacks.onToggleReplay = [this] { toggleReplay(); };
    overlayCallbacks.onSaveClip = [this] { saveClip(); };
    overlayCallbacks.onToggleRecording = [this] { toggleRecording(); };
    overlayCallbacks.onOpenLibrary = [this] {
        if (library_) library_->open();
    };
    overlayCallbacks.onOpenSettings = [this] {
        if (settingsWindow_) settingsWindow_->open(settings_.settings());
    };
    overlayCallbacks.onRefresh = [this] { updateStatus(); };
    overlay_ = std::make_unique<OverlayWindow>(instance_, std::move(overlayCallbacks));

    toast_ = std::make_unique<ToastWindow>(instance_);

    TrayIcon::Callbacks trayCallbacks;
    trayCallbacks.onShow = [this] { overlay_->show(); };
    trayCallbacks.onSettings = [this] { settingsWindow_->open(settings_.settings()); };
    trayCallbacks.onLibrary = [this] { library_->open(); };
    trayCallbacks.onExit = [this] { requestExit(); };
    trayIcon_ = std::make_unique<TrayIcon>(std::move(trayCallbacks));
    if (!trayIcon_->create(mainWindow_->handle())) {
        QLOG_WARN(L"Bandeja indisponível; o painel continua no atalho.");
    }

    // A entrada de inicialização é refeita a cada abertura: acompanha a pasta do exe e o --background.
    if (settings_.settings().startWithWindows) startup::setEnabled(true);

    // Sem janela principal: a interface é o painel (Alt+P). Aberto pelo usuário, o Qlip já mostra o painel;
    // na inicialização com o Windows, só um aviso no canto.
    registerHotkeys();
    const bool background = std::wstring_view(GetCommandLineW()).find(startup::kBackgroundArg) != std::wstring_view::npos;
    if (background) {
        setNotice(L"Qlip pronto  ·  " + SettingsWindow::keyName(settings_.settings().hotkeyOverlay) + L" abre o painel");
        updateStatus();
    } else {
        updateStatus();
        overlay_->show();
    }

    MSG msg{};
    int exitCode = 0;
    for (;;) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result == -1) {
            QLOG_ERROR(L"Erro no loop de mensagens.");
            exitCode = 1;
            break;
        }
        if (result == 0) {
            exitCode = static_cast<int>(msg.wParam);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Encerramento limpo: para gravação e replay, espera a finalização e libera os atalhos.
    if (recording_ && recording_->isRecording()) recording_->stop();
    if (replay_ && replay_->isActive()) replay_->stop();
    if (recording_) recording_->waitForIdle();
    if (replay_) replay_->waitForIdle();
    UnregisterHotKey(mainWindow_->handle(), kHotkeyRecord);
    UnregisterHotKey(mainWindow_->handle(), kHotkeyReplay);
    UnregisterHotKey(mainWindow_->handle(), kHotkeySave);
    UnregisterHotKey(mainWindow_->handle(), kHotkeyOverlay);

    QLOG_INFO(L"Qlip encerrando.");
    return exitCode;
}

void Application::registerHotkeys() {
    const Settings& s = settings_.settings();
    struct Binding {
        int id;
        unsigned hotkey;
        const wchar_t* action;
    };
    const Binding bindings[] = {
        {kHotkeyRecord, s.hotkeyRecord, L"gravar"},
        {kHotkeyReplay, s.hotkeyReplay, L"replay"},
        {kHotkeySave, s.hotkeySave, L"salvar clipe"},
        {kHotkeyOverlay, s.hotkeyOverlay, L"painel no jogo"},
    };

    // Solta todos antes: trocar teclas entre duas ações (ex.: F8 ↔ F9) falharia com a antiga ainda registrada.
    unregisterHotkeys();
    for (const Binding& binding : bindings) {
        if (!RegisterHotKey(mainWindow_->handle(), binding.id, hotkeyMods(binding.hotkey) | MOD_NOREPEAT, hotkeyVk(binding.hotkey))) {
            // Outro app já usa a tecla: informa em vez de falhar em silêncio.
            const std::wstring label = SettingsWindow::keyName(binding.hotkey) + L" (" + binding.action + L")";
            setNotice(label + L" em uso por outro aplicativo");
            QLOG_WARN(L"Não foi possível registrar o atalho " + label + L".");
        }
    }
}

void Application::unregisterHotkeys() {
    for (int id : {kHotkeyRecord, kHotkeyReplay, kHotkeySave, kHotkeyOverlay}) {
        UnregisterHotKey(mainWindow_->handle(), id);
    }
}

void Application::applySettings(const Settings& settings) {
    settings_.settings() = settings;
    registerHotkeys();
    if (!startup::setEnabled(settings.startWithWindows)) {
        QLOG_WARN(L"Não foi possível alterar a inicialização com o Windows.");
        MessageBoxW(mainWindow_->handle(), L"Não foi possível alterar a inicialização com o Windows.", L"Qlip", MB_OK | MB_ICONWARNING);
    }
    if (recording_ && recording_->isRecording()) {
        QLOG_INFO(L"Configurações salvas; valem a partir da próxima gravação.");
    }
    updateStatus();
}

RecordingManager::Options Application::captureOptions() const {
    const Settings& s = settings_.settings();
    RecordingManager::Options options;
    options.folder = settings_.recordingsFolder();
    options.captureWindow = s.captureWindow;
    options.monitorDevice = s.monitorDevice;
    options.micDeviceId = s.micDeviceId;
    options.excludeWindow = mainWindow_->handle();
    options.recordGameAudio = s.recordGameAudio;
    options.recordMicAudio = s.recordMicAudio;
    options.gameVolume = s.gameVolume;
    options.micVolume = s.micVolume;
    return options;
}

void Application::toggleRecording() {
    if (recording_->isRecording()) {
        stopRecording();
    } else if (!recording_->isFinalizing()) {
        startRecording();
    }
}

void Application::startRecording() {
    const Settings& s = settings_.settings();

    RecordingManager::Options options = captureOptions();
    options.fps = s.recordFps;
    options.height = s.resolutionHeight;
    options.bitrateKbps = qualityBitrateKbps(s.recordQuality);

    std::wstring error;
    if (!recording_->start(options, &error)) {
        MessageBoxW(mainWindow_->handle(), error.c_str(), L"Qlip", MB_OK | MB_ICONWARNING);
        mainWindow_->setStatus(L"Erro");
        mainWindow_->setDetail(L"Não foi possível iniciar a gravação");
        return;
    }

    mainWindow_->setRecording(true);
    setNotice(L"Gravando  ·  " + SettingsWindow::keyName(s.hotkeyRecord) + L" para parar");
    updateStatus();
}

void Application::stopRecording() {
    recording_->stop();
    mainWindow_->setRecording(false);
    setNotice(L"Finalizando a gravação…");
    updateStatus();
}

void Application::toggleReplay() {
    if (replay_->isActive()) {
        replay_->stop();
        setNotice(L"Replay desligado");
        updateStatus();
        return;
    }

    // Replay usa as configurações de clipe: FPS e qualidade próprios; a duração é a do replay.
    const Settings& s = settings_.settings();
    RecordingManager::Options options = captureOptions();
    options.fps = s.clipFps;
    options.height = s.resolutionHeight;
    options.bitrateKbps = qualityBitrateKbps(s.clipQuality);

    std::wstring error;
    if (!replay_->start(options, s.replaySeconds, &error)) {
        MessageBoxW(mainWindow_->handle(), error.c_str(), L"Qlip", MB_OK | MB_ICONWARNING);
        return;
    }
    setNotice(L"Replay ligado");
    updateStatus();
}

void Application::saveClip() {
    if (!replay_->isActive()) {
        setNotice(L"Ligue o replay com F10 antes de salvar clipes");
        updateStatus();
        return;
    }

    std::wstring error;
    if (!replay_->saveClip(settings_.clipsFolder(), &error)) {
        setNotice(error);
    } else {
        setNotice(L"Salvando clipe…");
    }
    updateStatus();
}

void Application::setNotice(const std::wstring& text) {
    std::lock_guard lock(eventMutex_);
    notice_ = text;
    ++noticeSeq_;
    noticeUntil_ = GetTickCount64() + kNoticeMs;
}

void Application::updateStatus() {
    const Settings& s = settings_.settings();
    std::wstring notice;
    uint64_t noticeSeq = 0;
    {
        std::lock_guard lock(eventMutex_);
        if (GetTickCount64() < noticeUntil_) notice = notice_;
        noticeSeq = noticeSeq_;
    }

    const bool recording = recording_->isRecording();
    const bool finalizing = recording_->isFinalizing();
    const bool replay = replay_->isActive();

    if (recording) {
        const auto total = static_cast<uint64_t>(recording_->elapsedSeconds());
        std::wstring detail = recording_->lastWarning();
        if (detail.empty()) {
            detail = L"Frames perdidos: " + std::to_wstring(recording_->droppedFrames()) +
                     (recording_->hasAudio() ? L"  ·  com áudio" : L"  ·  sem áudio");
        } else {
            detail = L"Aviso: " + detail;
        }
        mainWindow_->setState(MainWindow::RecordState::Recording);
        mainWindow_->setStatus(L"Gravando");
        mainWindow_->setTimer(hhmmss(total));
        mainWindow_->setDetail(notice.empty() ? detail : notice);
    } else if (finalizing) {
        mainWindow_->setState(MainWindow::RecordState::Finalizing);
        mainWindow_->setStatus(L"Finalizando");
        mainWindow_->setDetail(L"Salvando o arquivo…");
    } else if (replay) {
        // Sem contador: o replay guarda sempre os últimos N segundos; a tela só mostra quantos.
        mainWindow_->setState(MainWindow::RecordState::Replay);
        mainWindow_->setStatus(L"Replay ativo");
        mainWindow_->setTimer(L"Últimos " + std::to_wstring(s.replaySeconds) + L" s");
        mainWindow_->setDetail(notice.empty() ? SettingsWindow::keyName(s.hotkeySave) + L" salva o clipe  ·  " +
                                                    SettingsWindow::keyName(s.hotkeyOverlay) + L" abre o painel"
                                              : notice);
    } else {
        mainWindow_->setState(MainWindow::RecordState::Idle);
        mainWindow_->setStatus(L"Parado");
        mainWindow_->setTimer(L"00:00:00");
        mainWindow_->setDetail(notice.empty() ? SettingsWindow::keyName(s.hotkeyRecord) + L" grava  ·  " +
                                                    SettingsWindow::keyName(s.hotkeyReplay) + L" liga o replay  ·  " +
                                                    SettingsWindow::keyName(s.hotkeyOverlay) + L" painel"
                                              : notice);
    }

    // O cronômetro roda enquanto há gravação ou finalização; no replay, só para o aviso temporário expirar.
    mainWindow_->setTicking(recording || finalizing || replay);

    // Aviso novo com o painel fechado: mostra no canto da tela (não há janela principal).
    if (noticeSeq != toastSeq_ && !notice.empty()) {
        toastSeq_ = noticeSeq;
        if (toast_ && !(overlay_ && overlay_->isVisible())) {
            const bool bad = notice.rfind(L"Erro", 0) == 0 || notice.find(L"em uso") != std::wstring::npos ||
                             notice.rfind(L"Ligue", 0) == 0 || notice.rfind(L"Aguarde", 0) == 0;
            toast_->show(notice, bad ? theme::kWarning : (recording ? theme::kRecording : theme::kAccent));
        }
    }

    if (overlay_ && overlay_->isVisible()) {
        OverlayWindow::Status status;
        status.replayOn = replay;
        status.replaySeconds = s.replaySeconds;
        status.recording = recording;
        status.finalizing = finalizing;
        status.recordingSeconds = recording ? static_cast<unsigned long long>(recording_->elapsedSeconds()) : 0;
        status.notice = notice;
        status.keyReplay = SettingsWindow::keyName(s.hotkeyReplay);
        status.keySave = SettingsWindow::keyName(s.hotkeySave);
        status.keyRecord = SettingsWindow::keyName(s.hotkeyRecord);
        status.keyOverlay = SettingsWindow::keyName(s.hotkeyOverlay);
        overlay_->setStatus(status);
    }
}

void Application::handleRecordingEvent() {
    std::wstring fatal;
    std::wstring replayFatal;
    {
        std::lock_guard lock(eventMutex_);
        fatal.swap(pendingFatal_);
        replayFatal.swap(pendingReplayFatal_);
    }

    if (!fatal.empty()) {
        // A gravação parou sozinha: encerra com o que já foi gravado e avisa o usuário.
        if (recording_->isRecording()) stopRecording();
        MessageBoxW(mainWindow_->handle(), (fatal + L"\n\nO que já foi gravado foi salvo.").c_str(), L"Qlip", MB_OK | MB_ICONWARNING);
    }

    if (!replayFatal.empty()) {
        if (replay_->isActive()) replay_->stop();
        MessageBoxW(mainWindow_->handle(), (L"O replay foi desligado: " + replayFatal).c_str(), L"Qlip", MB_OK | MB_ICONWARNING);
    }

    if (!recording_->isRecording() && !recording_->isFinalizing()) {
        mainWindow_->setRecording(false);
        const auto file = recording_->lastFile();
        if (!file.empty()) {
            setNotice(L"Salvo: " + file.filename().wstring());
        } else if (!recording_->lastError().empty()) {
            setNotice(L"Erro: " + recording_->lastError());
        }
    }
    updateStatus();
}
