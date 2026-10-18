#include "replay/ReplayManager.h"

#include <chrono>
#include <memory>

#include <windows.h>

#include "utils/Logger.h"
#include "utils/Text.h"

namespace {

std::wstring clipName() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[80]{};
    swprintf_s(buffer, L"Qlip_Clipe_%04d-%02d-%02d_%02d-%02d-%02d-%03d.mp4", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}

}  // namespace

ReplayManager::ReplayManager(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

ReplayManager::~ReplayManager() {
    stop();
    waitForIdle();
}

bool ReplayManager::start(const RecordingManager::Options& options, int seconds, std::wstring* error) {
    if (isActive()) {
        if (error) *error = L"O replay já está ativo.";
        return false;
    }
    waitForIdle();

    seconds_ = seconds;
    buffer_.clear();
    buffer_.setDurationSeconds(seconds);

    RecordingManager::Options pipeline = options;
    pipeline.replayBuffer = &buffer_;

    RecordingManager::Callbacks recordingCallbacks;
    recordingCallbacks.onFatal = [this](const std::wstring& reason) {
        if (callbacks_.onFatal) callbacks_.onFatal(reason);
    };
    recorder_ = std::make_unique<RecordingManager>(std::move(recordingCallbacks));
    if (!recorder_->start(pipeline, error)) {
        recorder_.reset();
        buffer_.clear();
        return false;
    }
    QLOG_INFO(L"Replay ativo: últimos " + std::to_wstring(seconds) + L" s.");
    return true;
}

void ReplayManager::stop() {
    if (recorder_) recorder_->stop();
}

void ReplayManager::waitForIdle() {
    if (recorder_) recorder_->waitForIdle();
    // Só limpa depois do pipeline inteiro: o writer ainda pode estar empurrando pacotes até aqui.
    buffer_.clear();
    std::lock_guard lock(saverMutex_);
    if (saver_.joinable()) saver_.join();
}

bool ReplayManager::isActive() const {
    return recorder_ && recorder_->isRecording();
}

bool ReplayManager::saveClip(const std::filesystem::path& folder, std::wstring* error) {
    auto fail = [error](const std::wstring& message) {
        if (error) *error = message;
        return false;
    };
    if (!isActive()) return fail(L"O replay está desligado. Pressione F10 para ligar.");
    if (saving_) return fail(L"Aguarde o clipe anterior terminar de salvar.");

    // Cópia dos pacotes na thread da UI: rápida (só clones de pacotes já codificados).
    auto clip = std::make_shared<ReplayBuffer::Clip>();
    if (!buffer_.snapshot(seconds_, *clip)) {
        return fail(L"Ainda não há vídeo suficiente no buffer. Aguarde alguns segundos.");
    }

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) return fail(L"Não foi possível criar a pasta de clipes: " + fromUtf8(ec.message()));
    const std::filesystem::path file = folder / clipName();

    // Com saving_ falso, a thread anterior já terminou: o join é imediato e não espera o disco.
    {
        std::lock_guard lock(saverMutex_);
        if (saver_.joinable()) saver_.join();
    }
    saving_ = true;
    saver_ = std::thread([this, clip, file] {
        // O áudio entra no buffer com a folga do mixer (300 ms) de atraso: espera ela passar e copia de novo,
        // para o fim do clipe ter som. Se o replay foi desligado nesse meio-tempo, fica a primeira cópia.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        auto fresh = std::make_shared<ReplayBuffer::Clip>();
        // Cópia nova terminando antes da primeira = o replay foi religado (outra sessão, tempo recomeçou): não serve.
        const bool useFresh = buffer_.snapshot(seconds_, *fresh) && !fresh->entries.empty() && !clip->entries.empty() &&
                              fresh->entries.back().timeUs >= clip->entries.back().timeUs;
        const ReplayBuffer::Clip& source = useFresh ? *fresh : *clip;
        std::string writeError;
        const bool ok = clip::save(source, file, &writeError);
        // Libera a flag antes de avisar: quando a UI receber o aviso, um novo clipe já pode ser pedido.
        saving_ = false;
        if (ok) {
            QLOG_INFO(L"Clipe salvo: " + file.wstring());
            if (callbacks_.onClipSaved) callbacks_.onClipSaved(file);
        } else {
            QLOG_ERROR(L"Falha ao salvar clipe: " + fromUtf8(writeError));
            if (callbacks_.onClipError) callbacks_.onClipError(fromUtf8(writeError));
        }
    });
    return true;
}
