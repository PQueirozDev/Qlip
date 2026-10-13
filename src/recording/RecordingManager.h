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

#include <windows.h>

#include "audio/AudioRing.h"
#include "audio/AudioSource.h"
#include "capture/ScreenCapture.h"
#include "encoder/AudioEncoder.h"
#include "encoder/VideoEncoder.h"
#include "recording/VideoMuxer.h"
#include "replay/ReplayBuffer.h"
#include "utils/BoundedQueue.h"

struct AVPacket;

// Orquestra a gravação contínua:
//   vídeo:  captura (WGC, monitor ou janela) -> encoder NVENC (CFR) ------\
//   áudio:  WASAPI (jogo e/ou mic) -> mixer -> AAC ----------------------> writer -> MKV -> MP4
// Ao parar, a finalização roda em outra thread, para que a interface não trave.
class RecordingManager {
public:
    struct Options {
        int fps = 60;
        int height = 1080;          // 0 = resolução original do alvo
        int bitrateKbps = 12000;
        std::filesystem::path folder;
        bool captureWindow = false; // false = monitor; true = janela (jogo) em primeiro plano
        std::wstring monitorDevice; // monitor a gravar (vazio = principal)
        std::wstring micDeviceId;   // microfone (vazio = padrão do Windows)
        HWND excludeWindow = nullptr;  // a própria janela do Qlip nunca é escolhida como alvo
        bool recordGameAudio = false;
        bool recordMicAudio = false;
        int gameVolume = 100;       // %
        int micVolume = 100;        // %
        // Se definido, o pipeline não grava arquivo: os pacotes vão para o buffer de replay.
        ReplayBuffer* replayBuffer = nullptr;
    };

    struct Callbacks {
        // Chamado na thread de finalização quando o arquivo final está pronto (ou falhou).
        std::function<void()> onFinished;
        // Chamado quando a gravação precisa parar sozinha (alvo sumiu, disco cheio, falha de codificação).
        // Pode vir de qualquer thread: quem recebe deve sinalizar a UI e chamar stop() de lá.
        std::function<void(const std::wstring& reason)> onFatal;
    };

    explicit RecordingManager(Callbacks callbacks);
    ~RecordingManager();

    bool start(const Options& options, std::wstring* error);
    // Não bloqueia: encerra a captura e agenda a finalização.
    void stop();
    // Espera a finalização terminar (usado ao encerrar o app).
    void waitForIdle();

    bool isRecording() const { return recording_; }
    bool isFinalizing() const { return finalizing_; }
    bool hasAudio() const { return audioActive_; }
    double elapsedSeconds() const;
    uint64_t droppedFrames() const { return dropped_; }
    std::filesystem::path lastFile() const;
    std::wstring lastError() const;
    // Avisos da última gravação que não impediram o início (ex.: microfone indisponível).
    std::wstring lastWarning() const;

    // Converte arquivos .recording.mkv que sobraram de uma queda em .mp4.
    static void recoverLeftovers(const std::filesystem::path& folder);

private:
    struct Frame {
        int slot = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t qpcTicks = 0;
    };

    void onCaptureFrame(const ScreenCapture::Frame& frame);
    void onCaptureFatal(const std::wstring& reason);
    void encoderLoop();
    void audioLoop();
    void writerLoop();
    void finalize();
    void finishProducer();
    void mixBlock(int64_t startFrame, float* mix, float* scratch) const;
    void setError(const std::wstring& message);
    void setWarning(const std::wstring& message);
    void fail(const std::wstring& reason);
    bool hasEnoughDiskSpace(const std::filesystem::path& folder, uint64_t minBytes) const;
    void cleanupAfterFailedStart();

    Callbacks callbacks_;
    ScreenCapture capture_;
    VideoEncoder encoder_;
    AudioEncoder audioEncoder_;
    VideoMuxer muxer_;

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

    // Quantos produtores ainda escrevem em packets_ (vídeo e, se houver, áudio). O último fecha a fila.
    std::atomic<int> producersLeft_{0};

    std::atomic<bool> recording_{false};
    std::atomic<bool> finalizing_{false};
    // Instante (em amostras de áudio) em que stop() foi chamado; -1 enquanto a gravação está ativa.
    std::atomic<int64_t> stopFrame_{-1};
    std::atomic<bool> writeFailed_{false};
    std::atomic<uint64_t> dropped_{0};

    int64_t startTicks_ = 0;
    int64_t frequency_ = 1;
    int64_t frameIntervalTicks_ = 1;  // 1/FPS em ticks do QPC (saída CFR)
    bool timerResolutionRaised_ = false;
    std::filesystem::path tempFile_;
    std::filesystem::path finalFile_;
    ReplayBuffer* replay_ = nullptr;  // modo replay: pacotes vão para o buffer em vez do arquivo

    mutable std::mutex stateMutex_;
    std::filesystem::path lastFile_;
    std::wstring lastError_;
    std::wstring lastWarning_;
};
