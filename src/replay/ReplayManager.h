#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "recording/RecordingManager.h"
#include "replay/ReplayBuffer.h"

// Replay instantâneo: mantém os últimos N segundos codificados em memória (sem arquivo) e salva
// um clipe sob demanda. O pipeline é um RecordingManager no modo replay, então roda junto com a gravação.
class ReplayManager {
public:
    struct Callbacks {
        // Chamados da thread de salvamento quando o clipe fica pronto ou falha.
        std::function<void(const std::filesystem::path& file)> onClipSaved;
        std::function<void(const std::wstring& error)> onClipError;
        // Chamado quando o replay para sozinho (ex.: a tela sumiu). Pode vir de qualquer thread.
        std::function<void(const std::wstring& reason)> onFatal;
    };

    explicit ReplayManager(Callbacks callbacks);
    ~ReplayManager();

    // options: FPS, qualidade, alvo e áudio do clipe. seconds: duração do buffer.
    bool start(const RecordingManager::Options& options, int seconds, std::wstring* error);
    void stop();
    // Espera a finalização do pipeline e o salvamento em andamento (usado ao encerrar).
    void waitForIdle();

    bool isActive() const;
    int seconds() const { return seconds_; }
    double bufferedSeconds() const { return buffer_.bufferedSeconds(); }

    // Salva os últimos segundos do buffer em folder. Retorna logo; o arquivo é escrito em outra thread.
    bool saveClip(const std::filesystem::path& folder, std::wstring* error);

private:
    Callbacks callbacks_;
    ReplayBuffer buffer_;
    std::unique_ptr<RecordingManager> recorder_;
    std::thread saver_;
    std::mutex saverMutex_;
    // Verdadeiro enquanto um clipe está sendo escrito: um novo pedido é recusado (não espera na UI).
    std::atomic<bool> saving_{false};
    int seconds_ = 30;
};
