#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "recording/CapturePipeline.h"
#include "recording/PacketSink.h"
#include "replay/ReplayBuffer.h"

// Replay instantâneo: mantém os últimos N segundos codificados em memória (sem arquivo) e salva
// um clipe sob demanda. É um destino de um CapturePipeline, próprio ou compartilhado com a gravação.
class ReplayManager : public PacketSink {
public:
    struct Callbacks {
        // Chamados da thread de salvamento quando o clipe fica pronto ou falha.
        std::function<void(const std::filesystem::path& file)> onClipSaved;
        std::function<void(const std::wstring& error)> onClipError;
    };

    explicit ReplayManager(Callbacks callbacks);
    ~ReplayManager() override;

    // pipeline: já iniciado, com o FPS, a qualidade, o alvo e o áudio do clipe. seconds: duração do buffer.
    bool start(const std::shared_ptr<CapturePipeline>& pipeline, int seconds, std::wstring* error);
    // Desanexa do pipeline (que continua, se a gravação o usa) e descarta o buffer.
    void stop();
    // Espera o salvamento em andamento (usado ao encerrar).
    void waitForIdle();
    // Pipeline em uso (nullptr com o replay desligado). Só a thread da UI usa.
    CapturePipeline* pipeline() const { return pipeline_.get(); }

    bool isActive() const;
    int seconds() const { return seconds_; }
    double bufferedSeconds() const { return buffer_.bufferedSeconds(); }

    // Salva os últimos segundos do buffer em folder. Retorna logo; o arquivo é escrito em outra thread.
    bool saveClip(const std::filesystem::path& folder, std::wstring* error);

    // PacketSink (thread de escrita do pipeline).
    void onPacket(const AVPacket* packet) override;
    void onEnd() override { active_ = false; }

private:
    Callbacks callbacks_;
    ReplayBuffer buffer_;
    std::shared_ptr<CapturePipeline> pipeline_;
    // Ligado enquanto anexado; o pipeline desliga (onEnd) se parar sozinho.
    std::atomic<bool> active_{false};
    std::thread saver_;
    std::mutex saverMutex_;
    // Verdadeiro enquanto um clipe está sendo escrito: um novo pedido é recusado (não espera na UI).
    std::atomic<bool> saving_{false};
    int seconds_ = 30;
};
