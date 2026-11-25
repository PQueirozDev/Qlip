#include "recording/CapturePipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}

#include "capture/Monitors.h"
#include "utils/Logger.h"
#include "utils/Text.h"

namespace {

// Logs do FFmpeg vão para o Logger do Qlip: um app GUI não tem stderr para ver o av_log.
void ffmpegLogCallback(void* /*ptr*/, int level, const char* format, va_list args) {
    if (level > AV_LOG_WARNING) return;
    char buffer[512]{};
    vsnprintf(buffer, sizeof(buffer), format, args);
    std::string text(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    const auto message = L"FFmpeg: " + fromUtf8(text);
    if (level <= AV_LOG_ERROR) {
        QLOG_ERROR(message);
    } else {
        QLOG_WARN(message);
    }
}

constexpr int kSlotCount = 4;          // buffers de GPU em voo (capture -> encoder), um deles segurado
constexpr size_t kPacketQueue = 120;   // pacotes em fila (writer): ~2 s de vídeo a 60 fps
constexpr int kAudioBitrateKbps = 192;
// Folga antes de misturar: o que uma fonte entregar com até 300 ms de atraso (jogo pesado, driver USB) ainda entra.
constexpr int64_t kAudioMarginFrames = AudioEncoder::kSampleRate * 3 / 10;

int64_t qpcNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Janela candidata a alvo: visível, com título, não é ferramenta/shell, e não pertence à própria janela do Qlip.
bool isCapturableWindow(HWND window, HWND exclude) {
    if (!window || window == exclude) return false;
    if (!IsWindowVisible(window) || IsIconic(window)) return false;
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (exclude && GetWindow(window, GW_OWNER) == exclude) return false;
    if (GetWindowTextLengthW(window) == 0) return false;

    wchar_t className[64]{};
    GetClassNameW(window, className, 64);
    if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 || wcscmp(className, L"Shell_TrayWnd") == 0) {
        return false;
    }

    RECT rect{};
    GetWindowRect(window, &rect);
    return (rect.right - rect.left) >= 64 && (rect.bottom - rect.top) >= 64;
}

}  // namespace

bool CapturePipeline::Options::sameOutput(const Options& other) const {
    return fps == other.fps && height == other.height && bitrateKbps == other.bitrateKbps &&
           captureWindow == other.captureWindow && monitorDevice == other.monitorDevice &&
           recordGameAudio == other.recordGameAudio && recordMicAudio == other.recordMicAudio &&
           (!recordMicAudio || micDeviceId == other.micDeviceId) && gameVolume == other.gameVolume &&
           micVolume == other.micVolume;
}

HWND CapturePipeline::findTargetWindow(HWND exclude) {
    // A que está em primeiro plano; se for o próprio Qlip, a de cima na ordem Z.
    HWND foreground = GetForegroundWindow();
    if (isCapturableWindow(foreground, exclude)) return foreground;
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (isCapturableWindow(window, exclude)) return window;
    }
    return nullptr;
}

CapturePipeline::CapturePipeline(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

CapturePipeline::~CapturePipeline() {
    stop();
    waitForIdle();
}

bool CapturePipeline::start(const Options& options, std::wstring* error) {
    auto failStart = [error](const std::wstring& message) {
        if (error) *error = message;
        QLOG_ERROR(message);
        return false;
    };

    if (running_) return failStart(L"A captura já está em andamento.");
    waitForIdle();
    options_ = options;

    // Zero do tempo: vídeo e áudio medem a partir daqui, com o mesmo QPC.
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&frequency_));
    frameIntervalTicks_ = frequency_ / std::max(1, options.fps);
    startTicks_ = qpcNow();

    // Sem isso o Windows acorda threads só a cada ~15 ms, e o relógio CFR de 60 fps ficaria irregular.
    if (timeBeginPeriod(1) == TIMERR_NOERROR) timerResolutionRaised_ = true;

    av_log_set_callback(ffmpegLogCallback);
    failed_ = false;
    dropped_ = 0;
    {
        std::lock_guard lock(stateMutex_);
        warning_.clear();
    }
    {
        std::lock_guard lock(sinksMutex_);
        sinks_.clear();
    }
    frames_ = std::make_unique<BoundedQueue<Frame>>(kSlotCount);
    packets_ = std::make_unique<BoundedQueue<AVPacket*>>(kPacketQueue);

    // Alvo: o monitor escolhido (ou o principal) ou uma janela.
    HMONITOR monitor = findMonitor(options.monitorDevice);
    if (!monitor) {
        if (!options.monitorDevice.empty()) setWarning(L"O monitor escolhido não está conectado; gravando o principal.");
        monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    targetWindow_ = nullptr;
    if (options.captureWindow) {
        targetWindow_ = findTargetWindow(options.excludeWindow);
        if (!targetWindow_) {
            cleanupAfterFailedStart();
            return failStart(L"Nenhuma janela encontrada para gravar. Abra o jogo ou escolha a área de trabalho nas configurações.");
        }
    }

    ScreenCapture::Callbacks captureCallbacks;
    captureCallbacks.onFrame = [this](const ScreenCapture::Frame& frame) { onCaptureFrame(frame); };
    captureCallbacks.onFatal = [this](const std::wstring& reason) { fail(reason); };

    std::wstring captureError;
    if (!capture_.start(monitor, targetWindow_, options.fps, kSlotCount, std::move(captureCallbacks), &captureError)) {
        cleanupAfterFailedStart();
        return failStart(captureError);
    }

    // Tamanho de saída: o do alvo, limitado pela altura escolhida (dimensões pares para o H.264).
    const int srcWidth = static_cast<int>(capture_.width());
    const int srcHeight = static_cast<int>(capture_.height());
    int targetHeight = (options.height <= 0 || options.height >= srcHeight) ? srcHeight : options.height;
    targetHeight &= ~1;
    int targetWidth = static_cast<int>(static_cast<int64_t>(srcWidth) * targetHeight / std::max(1, srcHeight));
    targetWidth &= ~1;

    // Áudio: cada fonte é opcional. Falha de uma fonte vira aviso, e a captura segue.
    gameRing_.reset();
    micRing_.reset();
    gameGain_ = std::clamp(options.gameVolume, 0, 200) / 100.0f;
    micGain_ = std::clamp(options.micVolume, 0, 200) / 100.0f;
    if (options.recordGameAudio) {
        gameSource_ = std::make_unique<AudioSource>(AudioSource::Kind::GameLoopback, gameRing_);
        std::wstring audioError;
        if (!gameSource_->start(startTicks_, frequency_, &audioError)) {
            gameSource_.reset();
            setWarning(L"Áudio do jogo indisponível: " + audioError);
        }
    }
    if (options.recordMicAudio) {
        micSource_ = std::make_unique<AudioSource>(AudioSource::Kind::Microphone, micRing_, options.micDeviceId);
        std::wstring audioError;
        if (!micSource_->start(startTicks_, frequency_, &audioError)) {
            micSource_.reset();
            setWarning(L"Microfone indisponível: " + audioError);
        }
    }
    bool wantAudio = gameSource_ || micSource_;
    if (wantAudio) {
        std::string encoderError;
        if (!audioEncoder_.open(kAudioBitrateKbps, &encoderError)) {
            setWarning(L"Áudio desativado: " + fromUtf8(encoderError));
            gameSource_.reset();
            micSource_.reset();
            wantAudio = false;
        }
    }

    VideoEncoder::Config encoderConfig;
    encoderConfig.width = targetWidth;
    encoderConfig.height = targetHeight;
    encoderConfig.fps = options.fps;
    encoderConfig.bitrateKbps = options.bitrateKbps;

    std::string encoderError;
    if (!encoder_.open(encoderConfig, capture_.device(), &encoderError)) {
        cleanupAfterFailedStart();
        return failStart(fromUtf8(encoderError));
    }
    // A captura já entrega frames: se o alvo sumiu durante a inicialização, o start falha aqui.
    if (failed_) {
        cleanupAfterFailedStart();
        return failStart(L"A captura parou durante a inicialização (a tela ou a janela sumiu).");
    }

    audioActive_ = wantAudio;
    producersLeft_ = wantAudio ? 2 : 1;
    stopFrame_ = -1;
    running_ = true;
    encoderThread_ = std::thread([this] { encoderLoop(); });
    writerThread_ = std::thread([this] { writerLoop(); });
    if (wantAudio) audioThread_ = std::thread([this] { audioLoop(); });

    QLOG_INFO(L"Captura iniciada: " + std::to_wstring(targetWidth) + L"x" + std::to_wstring(targetHeight) + L" @ " +
              std::to_wstring(options.fps) + L" fps (CFR), " + std::to_wstring(options.bitrateKbps) + L" kbps, áudio: " +
              (wantAudio ? L"sim" : L"não"));
    return true;
}

void CapturePipeline::cleanupAfterFailedStart() {
    capture_.stop();
    gameSource_.reset();
    micSource_.reset();
    encoder_.close();
    audioEncoder_.close();
    frames_.reset();
    packets_.reset();
    audioActive_ = false;
    if (timerResolutionRaised_) {
        timeEndPeriod(1);
        timerResolutionRaised_ = false;
    }
}

void CapturePipeline::stop() {
    if (!running_.exchange(false)) return;
    stopFrame_ = (qpcNow() - startTicks_) * AudioEncoder::kSampleRate / frequency_;

    // Ordem importante: para a captura (nenhum frame novo entra) e fecha a fila de frames.
    // O encoder de vídeo para os ticks e faz flush; o áudio faz o último bloco e flush; o writer termina.
    capture_.stop();
    if (frames_) frames_->close();
    wake_.notify_all();

    finalizing_ = true;
    QLOG_INFO(L"Captura parada; finalizando.");
    finalizer_ = std::thread([this] { finalize(); });
}

void CapturePipeline::waitForIdle() {
    if (finalizer_.joinable()) finalizer_.join();
}

int64_t CapturePipeline::nowUs() const {
    return (qpcNow() - startTicks_) * 1000000 / std::max<int64_t>(1, frequency_);
}

std::wstring CapturePipeline::warning() const {
    std::lock_guard lock(stateMutex_);
    return warning_;
}

void CapturePipeline::addSink(PacketSink* sink) {
    {
        std::lock_guard lock(sinksMutex_);
        // Writer ainda ativo: o destino passa a receber a partir do próximo pacote.
        if (packets_ && !packets_->isClosed()) {
            sinks_.push_back(sink);
            return;
        }
    }
    // O pipeline já terminou (ex.: falhou logo antes): o destino é encerrado na hora, sem ficar esperando.
    sink->onEnd();
}

bool CapturePipeline::removeSink(PacketSink* sink) {
    std::lock_guard lock(sinksMutex_);
    const auto it = std::find(sinks_.begin(), sinks_.end(), sink);
    if (it == sinks_.end()) return false;
    sinks_.erase(it);
    return true;
}

size_t CapturePipeline::sinkCount() const {
    std::lock_guard lock(sinksMutex_);
    return sinks_.size();
}

void CapturePipeline::onCaptureFrame(const ScreenCapture::Frame& frame) {
    if (frame.slot < 0) {
        ++dropped_;  // sem slot livre: o encoder está atrasado
        return;
    }
    Frame item;
    item.slot = frame.slot;
    item.width = frame.width;
    item.height = frame.height;
    item.qpcTicks = frame.qpcTicks;

    if (!frames_ || !frames_->tryPush(item)) {
        capture_.releaseSlot(frame.slot);
        ++dropped_;
    }
}

void CapturePipeline::finishProducer() {
    // O último produtor a terminar fecha a fila: o writer só termina quando vídeo e áudio acabaram.
    if (producersLeft_.fetch_sub(1) == 1) packets_->close();
}

void CapturePipeline::encoderLoop() {
    // Saída CFR: a cada tick de 1/FPS o encoder recebe o frame mais recente. Se a tela não mudou,
    // o último frame é repetido. O slot do frame segurado não volta ao pool até chegar outro.
    auto sink = [this](AVPacket* packet) {
        packet->stream_index = kVideoStream;
        // Duração explícita (µs): o MP4 fragmentado não precisa estimar a do último frame de cada fragmento.
        if (packet->duration <= 0) packet->duration = frequency_ > 0 ? frameIntervalTicks_ * 1000000 / frequency_ : 0;
        AVPacket* copy = av_packet_clone(packet);
        if (!copy) return false;
        if (!packets_->push(copy)) {
            av_packet_free(&copy);
            return false;
        }
        return true;
    };

    Frame held;
    bool haveHeld = false;
    int64_t nextTick = startTicks_ + frameIntervalTicks_;

    auto emit = [&](int64_t tickTicks) {
        if (failed_) return;
        const int64_t pts = std::max<int64_t>(0, (tickTicks - startTicks_) * 1000000 / frequency_);
        // Conversão na GPU com o contexto travado; o envio ao NVENC fica fora do lock.
        // O slot não é liberado: o frame segurado será repetido nos próximos ticks.
        const bool converted = capture_.useSlot(held.slot, [&](ID3D11Texture2D* texture) {
            return encoder_.convert(texture, static_cast<int>(held.width), static_cast<int>(held.height));
        });
        const bool ok = converted && encoder_.encodeConverted(pts, sink);
        if (!ok && !failed_) fail(L"Falha ao codificar o vídeo com NVENC.");
    };

    for (;;) {
        // Checa o fechamento a cada volta: se o encode demorar mais que um tick, o ramo de tick
        // nunca chega ao popFor e o stop() ficaria sem sair do loop.
        if (frames_->isClosed()) break;

        const int64_t now = qpcNow();
        if (now >= nextTick) {
            // Pega o frame mais recente da fila antes de emitir: mesmo recuperando atraso (vários ticks seguidos),
            // a imagem acompanha a tela em vez de repetir sempre o mesmo frame.
            Frame pending;
            while (frames_->popFor(pending, std::chrono::microseconds(0)) == BoundedQueue<Frame>::PopResult::Item) {
                if (haveHeld) capture_.releaseSlot(held.slot);
                held = pending;
                haveHeld = true;
            }
            if (haveHeld) emit(nextTick);
            nextTick += frameIntervalTicks_;
            // Atraso curto (ex.: o primeiro encode do NVENC, que inicializa o encoder) é recuperado emitindo os
            // ticks em sequência, com o frame repetido: a linha do tempo de 60 fps não ganha buraco.
            // Só reancora se o atraso passar de 1 s (ex.: PC suspenso), para não gerar uma rajada enorme.
            if (nextTick < now - frequency_) nextTick = now;
            continue;
        }

        const int64_t waitTicks = nextTick - now;
        const auto waitUs = std::chrono::microseconds(waitTicks * 1000000 / frequency_);
        Frame incoming;
        const auto result = frames_->popFor(incoming, waitUs);
        if (result == BoundedQueue<Frame>::PopResult::Closed) break;
        if (result == BoundedQueue<Frame>::PopResult::Item) {
            // Chegou frame novo: o anterior sai do "segurado" (o novo vale para os próximos ticks).
            if (haveHeld) capture_.releaseSlot(held.slot);
            held = incoming;
            haveHeld = true;
        }
    }

    if (haveHeld) capture_.releaseSlot(held.slot);

    // Esvazia o encoder. Falha aqui = fim incompleto: avisa.
    if (!failed_ && !encoder_.flush(sink)) {
        fail(L"Falha ao finalizar a codificação do vídeo.");
    }
    finishProducer();
}

void CapturePipeline::mixBlock(int64_t startFrame, float* mix, float* scratch) const {
    constexpr int kValues = AudioEncoder::kFrameSize * AudioRing::kChannels;
    std::fill_n(mix, kValues, 0.0f);
    if (gameSource_) {
        gameRing_.read(startFrame, scratch, AudioEncoder::kFrameSize);
        for (int i = 0; i < kValues; ++i) mix[i] += scratch[i] * gameGain_;
    }
    if (micSource_) {
        micRing_.read(startFrame, scratch, AudioEncoder::kFrameSize);
        for (int i = 0; i < kValues; ++i) mix[i] += scratch[i] * micGain_;
    }
    for (int i = 0; i < kValues; ++i) mix[i] = std::clamp(mix[i], -1.0f, 1.0f);
}

void CapturePipeline::audioLoop() {
    // Mixer: emite blocos de 1024 amostras no tempo do pipeline, sempre com a folga de kAudioMarginFrames.
    // Assim cada fonte já escreveu o que chegou no seu intervalo, e não há espera por dispositivo.
    std::vector<float> mix(AudioEncoder::kFrameSize * AudioRing::kChannels);
    std::vector<float> scratch(AudioEncoder::kFrameSize * AudioRing::kChannels);

    auto sink = [this](AVPacket* packet) {
        packet->stream_index = kAudioStream;
        AVPacket* copy = av_packet_clone(packet);
        if (!copy) return false;
        if (!packets_->push(copy)) {
            av_packet_free(&copy);
            return false;
        }
        return true;
    };

    auto emitBlock = [&](int64_t start) {
        if (failed_) return;
        mixBlock(start, mix.data(), scratch.data());
        if (!audioEncoder_.encode(mix.data(), start, sink) && !failed_) {
            fail(L"Falha ao codificar o áudio.");
        }
    };
    auto waitTick = [this] {
        std::unique_lock lock(wakeMutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(5), [this] { return stopFrame_.load() >= 0; });
    };

    const int64_t kBlock = AudioEncoder::kFrameSize;
    int64_t next = 0;
    for (;;) {
        const int64_t nowFrame = (qpcNow() - startTicks_) * AudioEncoder::kSampleRate / frequency_;
        const int64_t stopFrame = stopFrame_.load();

        if (stopFrame < 0) {
            // Ativo: mistura só o que já tem a folga, para as fontes terem entregue os dados.
            while (next + kBlock <= nowFrame - kAudioMarginFrames) {
                emitBlock(next);
                next += kBlock;
            }
            waitTick();
            continue;
        }

        // Parando: espera a folga depois do stop (o WASAPI entrega com atraso), e então mistura até o instante do stop.
        // O último bloco é completado com silêncio, para não perder o fim.
        if (nowFrame < stopFrame + kAudioMarginFrames) {
            waitTick();
            continue;
        }
        while (next < stopFrame) {
            emitBlock(next);
            next += kBlock;
        }
        break;
    }

    if (!failed_ && !audioEncoder_.flush(sink)) fail(L"Falha ao finalizar o áudio.");
    finishProducer();
}

void CapturePipeline::deliver(const AVPacket* packet) {
    std::lock_guard lock(sinksMutex_);
    for (size_t i = 0; i < sinks_.size();) {
        PacketSink* sink = sinks_[i];
        sink->onPacket(packet);
        if (sink->finished()) {
            sinks_.erase(sinks_.begin() + static_cast<std::ptrdiff_t>(i));
            sink->onEnd();
        } else {
            ++i;
        }
    }
}

void CapturePipeline::endAllSinks() {
    std::vector<PacketSink*> ended;
    {
        std::lock_guard lock(sinksMutex_);
        ended.swap(sinks_);
    }
    for (PacketSink* sink : ended) sink->onEnd();
}

void CapturePipeline::writerLoop() {
    AVPacket* packet = nullptr;
    while (packets_->pop(packet)) {
        if (!failed_) deliver(packet);
        av_packet_free(&packet);
    }
    // A fila está fechada: addSink não anexa mais nada, e os destinos atuais recebem o último aviso.
    endAllSinks();
}

void CapturePipeline::finalize() {
    if (encoderThread_.joinable()) encoderThread_.join();
    if (audioThread_.joinable()) audioThread_.join();
    if (writerThread_.joinable()) writerThread_.join();

    // Estatísticas da sessão: permitem medir perdas de frame sem depender da barra de status.
    QLOG_INFO(L"Captura encerrada: " + std::to_wstring(stopFrame_.load() / AudioEncoder::kSampleRate) +
              L" s, frames perdidos: " + std::to_wstring(dropped_.load()));

    // Os mixers já terminaram: agora as fontes de áudio podem ser encerradas com segurança.
    gameSource_.reset();
    micSource_.reset();
    audioActive_ = false;

    if (timerResolutionRaised_) {
        timeEndPeriod(1);
        timerResolutionRaised_ = false;
    }

    capture_.stop();
    encoder_.close();
    audioEncoder_.close();
    finalizing_ = false;
    if (callbacks_.onIdle) callbacks_.onIdle(this);
}

void CapturePipeline::fail(const std::wstring& reason) {
    // Só a primeira falha vale; as seguintes são consequência (filas fechadas etc.).
    if (failed_.exchange(true)) return;

    QLOG_ERROR(reason);
    if (frames_) frames_->close();
    if (packets_) packets_->close();
    if (callbacks_.onFatal) callbacks_.onFatal(this, reason);
}

void CapturePipeline::setWarning(const std::wstring& message) {
    std::lock_guard lock(stateMutex_);
    if (!warning_.empty()) warning_ += L" ";
    warning_ += message;
    QLOG_WARN(message);
}
