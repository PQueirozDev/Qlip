#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "audio/AudioRing.h"
#include "audio/AudioSource.h"
#include "capture/ScreenCapture.h"
#include "encoder/AudioEncoder.h"
#include "encoder/VideoEncoder.h"
#include "recording/PacketSink.h"
#include "utils/BoundedQueue.h"

struct AVCodecContext;
struct AVPacket;

// Captura e codificação, uma sessão NVENC por pipeline:
//   vídeo:  captura (WGC, monitor ou janela) -> conversão e NVENC na GPU (CFR) --\
//   áudio:  WASAPI (jogo e/ou mic) -> mixer -> AAC ------------------------------> writer -> destinos
// Os destinos (arquivo de gravação, buffer de replay) entram e saem com o pipeline rodando: gravação e replay
// com as mesmas configurações compartilham um pipeline, em vez de abrir duas sessões de captura e de NVENC.
class CapturePipeline {
public:
    static constexpr int kVideoStream = 0;
    static constexpr int kAudioStream = 1;

    struct Options {
        int fps = 60;
        int height = 1080;          // 0 = resolução original do alvo
        int bitrateKbps = 12000;
        bool captureWindow = false; // false = monitor; true = janela (jogo) em primeiro plano
        std::wstring monitorDevice; // monitor a gravar (vazio = principal)
        std::wstring micDeviceId;   // microfone (vazio = padrão do Windows)
        HWND excludeWindow = nullptr;  // a própria janela do Qlip nunca é escolhida como alvo
        bool recordGameAudio = false;
        bool recordMicAudio = false;
        int gameVolume = 100;       // %
        int micVolume = 100;        // %

        // Mesma saída (vídeo, áudio e alvo): um pipeline com estas opções serve aos dois usos.
        bool sameOutput(const Options& other) const;
    };

    struct Callbacks {
        // O pipeline parou sozinho (alvo sumiu, falha de codificação). Pode vir de qualquer thread.
        std::function<void(CapturePipeline* pipeline, const std::wstring& reason)> onFatal;
        // A finalização terminou (threads encerradas, destinos avisados). Vem da thread de finalização.
        std::function<void(CapturePipeline* pipeline)> onIdle;
    };

    explicit CapturePipeline(Callbacks callbacks);
    ~CapturePipeline();

    CapturePipeline(const CapturePipeline&) = delete;
    CapturePipeline& operator=(const CapturePipeline&) = delete;

    bool start(const Options& options, std::wstring* error);
    // Não bloqueia: para a captura, esvazia os encoders e avisa os destinos (onEnd) em outra thread.
    void stop();
    void waitForIdle();

    bool isRunning() const { return running_; }
    // Sem threads ativas: pode ser destruído sem bloquear.
    bool isIdle() const { return !running_ && !finalizing_; }
    const Options& options() const { return options_; }
    // Janela capturada (nullptr = monitor).
    HWND targetWindow() const { return targetWindow_; }

    // Destinos. addSink/removeSink são síncronos com a thread de escrita: depois de removeSink, o destino
    // não recebe mais nada. Os destinos precisam viver até serem removidos ou receberem onEnd.
    void addSink(PacketSink* sink);
    bool removeSink(PacketSink* sink);
    size_t sinkCount() const;

    // O próximo frame de vídeo sai como IDR (início de gravação no meio do pipeline).
    void requestKeyframe() { encoder_.requestKeyframe(); }
    // Tempo do pipeline em microssegundos (a mesma base dos pacotes de vídeo).
    int64_t nowUs() const;

    // Parâmetros dos codecs, válidos enquanto o pipeline roda. audioCodec() é nullptr sem áudio.
    const AVCodecContext* videoCodec() const { return encoder_.codecContext(); }
    const AVCodecContext* audioCodec() const { return audioActive_ ? audioEncoder_.codecContext() : nullptr; }
    bool hasAudio() const { return audioActive_; }
    uint64_t droppedFrames() const { return dropped_; }
    // Avisos que não impediram o início (ex.: microfone indisponível).
    std::wstring warning() const;

    // Janela que seria capturada agora no modo "só o jogo": a de primeiro plano, ignorando a do Qlip.
    static HWND findTargetWindow(HWND exclude);

private:
    struct Frame {
        int slot = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t qpcTicks = 0;
    };

    void onCaptureFrame(const ScreenCapture::Frame& frame);
    void encoderLoop();
    void audioLoop();
    void writerLoop();
    void deliver(const AVPacket* packet);
    void endAllSinks();
    void finalize();
    void finishProducer();
    void mixBlock(int64_t startFrame, float* mix, float* scratch) const;
    void setWarning(const std::wstring& message);
    void fail(const std::wstring& reason);
    void cleanupAfterFailedStart();

    Callbacks callbacks_;
    Options options_;
    HWND targetWindow_ = nullptr;
    ScreenCapture capture_;
    VideoEncoder encoder_;
    AudioEncoder audioEncoder_;

    AudioRing gameRing_;
    AudioRing micRing_;
    std::unique_ptr<AudioSource> gameSource_;
    std::unique_ptr<AudioSource> micSource_;
    float gameGain_ = 1.0f;
    float micGain_ = 1.0f;
    std::atomic<bool> audioActive_{false};
    std::thread audioThread_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;

    std::unique_ptr<BoundedQueue<Frame>> frames_;
    std::unique_ptr<BoundedQueue<AVPacket*>> packets_;

    std::thread encoderThread_;
    std::thread writerThread_;
    std::thread finalizer_;

    mutable std::mutex sinksMutex_;
    std::vector<PacketSink*> sinks_;

    // Quantos produtores ainda escrevem em packets_ (vídeo e, se houver, áudio). O último fecha a fila.
    std::atomic<int> producersLeft_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> finalizing_{false};
    // Instante (em amostras de áudio) em que stop() foi chamado; -1 enquanto o pipeline está ativo.
    std::atomic<int64_t> stopFrame_{-1};
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> dropped_{0};

    int64_t startTicks_ = 0;
    int64_t frequency_ = 1;
    int64_t frameIntervalTicks_ = 1;  // 1/FPS em ticks do QPC (saída CFR)
    bool timerResolutionRaised_ = false;

    mutable std::mutex stateMutex_;
    std::wstring warning_;
};
