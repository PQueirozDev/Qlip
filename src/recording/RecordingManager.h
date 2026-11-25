#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "recording/CapturePipeline.h"
#include "recording/PacketSink.h"
#include "recording/VideoMuxer.h"

// Gravação contínua: um destino de arquivo ligado a um CapturePipeline (próprio ou compartilhado com o replay).
// Começa no primeiro IDR depois do start (pedido ao encoder na hora) e termina no instante do stop, com os tempos
// reescritos para começar em zero. Escreve MP4 fragmentado temporário e, ao final, faz o remux para MP4 normal
// em outra thread, para que nem a interface nem o pipeline travem.
class RecordingManager : public PacketSink {
public:
    struct Callbacks {
        // Chamado na thread de finalização quando o arquivo final está pronto (ou falhou).
        std::function<void()> onFinished;
        // Chamado quando a gravação precisa parar sozinha (disco cheio, falha de escrita).
        // Vem da thread de escrita do pipeline: quem recebe deve sinalizar a UI e chamar stop() de lá.
        std::function<void(const std::wstring& reason)> onFatal;
    };

    explicit RecordingManager(Callbacks callbacks);
    ~RecordingManager() override;

    // pipeline: já iniciado. O arquivo vai para folder.
    bool start(const std::shared_ptr<CapturePipeline>& pipeline, const std::filesystem::path& folder, std::wstring* error);
    // Não bloqueia: marca o fim da gravação no instante atual; a finalização roda em outra thread.
    void stop();
    // Espera a finalização terminar (usado ao encerrar o app). O pipeline precisa estar rodando ou já ter parado.
    void waitForIdle();

    bool isRecording() const;
    bool isFinalizing() const;
    bool hasAudio() const { return hasAudio_; }
    double elapsedSeconds() const;
    uint64_t droppedFrames() const;
    // Pipeline em uso enquanto grava (nullptr depois do stop). Só a thread da UI usa.
    CapturePipeline* pipeline() const { return pipeline_.get(); }
    std::filesystem::path lastFile() const;
    std::wstring lastError() const;
    // Avisos da última gravação que não impediram o início (ex.: microfone indisponível).
    std::wstring lastWarning() const;

    // Converte arquivos temporários que sobraram de uma queda em .mp4.
    static void recoverLeftovers(const std::filesystem::path& folder);

    // PacketSink (thread de escrita do pipeline).
    void onPacket(const AVPacket* packet) override;
    bool finished() const override { return done_; }
    void onEnd() override;

private:
    void write(const AVPacket* packet, int64_t offset);
    void failWrite(const std::wstring& reason);
    void finalize();
    void setError(const std::wstring& message);
    bool hasEnoughDiskSpace(const std::filesystem::path& folder, uint64_t minBytes) const;

    Callbacks callbacks_;
    VideoMuxer muxer_;
    std::shared_ptr<CapturePipeline> pipeline_;

    // Fim da gravação, no tempo do pipeline (µs): fica no máximo enquanto grava.
    std::atomic<int64_t> endUs_{INT64_MAX};
    // Estado da escrita: só a thread de escrita do pipeline usa.
    bool started_ = false;
    bool videoDone_ = false;
    bool audioDone_ = false;
    int64_t baseVideoUs_ = 0;
    int64_t baseAudioSamples_ = 0;
    int64_t written_ = 0;
    std::atomic<bool> done_{false};
    std::atomic<bool> writeFailed_{false};
    bool hasAudio_ = false;
    int64_t startTicks_ = 0;

    // Transições de estado (start, stop, onEnd, fim da finalização) sob stateMutex_.
    mutable std::mutex stateMutex_;
    std::condition_variable idle_;
    bool recording_ = false;
    bool finalizing_ = false;
    bool ended_ = false;
    std::thread finalizer_;
    std::filesystem::path tempFile_;
    std::filesystem::path finalFile_;
    std::filesystem::path lastFile_;
    std::wstring lastError_;
    std::wstring lastWarning_;
};
