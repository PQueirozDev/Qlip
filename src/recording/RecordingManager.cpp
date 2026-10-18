#include "recording/RecordingManager.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string_view>
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
constexpr uint64_t kStartMinFreeBytes = 1024ull * 1024 * 1024;  // 1 GB para iniciar
constexpr uint64_t kStopMinFreeBytes = 256ull * 1024 * 1024;    // 256 MB durante a gravação
constexpr int kDiskCheckEveryPackets = 300;
constexpr int kAudioBitrateKbps = 192;
// Folga antes de misturar: o que uma fonte entregar com até 300 ms de atraso (jogo pesado, driver USB) ainda entra.
constexpr int64_t kAudioMarginFrames = AudioEncoder::kSampleRate * 3 / 10;

int64_t qpcNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

std::wstring timestampName() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04d-%02d-%02d_%02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

// Arquivos temporários: Qlip_<data>.recording.mkv
constexpr std::wstring_view kTempSuffix = L".recording.mp4";
// Formato antigo (Matroska): ainda recuperado se sobrou de uma versão anterior.
constexpr std::wstring_view kLegacyTempSuffix = L".recording.mkv";

// Depois do remux: apaga o temporário se tudo foi copiado. Se algum pacote foi pulado (cortado/inválido),
// preserva o original como .damaged (fora dos sufixos de recuperação, para não ser reprocessado em loop).
void retireSource(const std::filesystem::path& source, int skippedPackets) {
    std::error_code ec;
    if (skippedPackets == 0) {
        std::filesystem::remove(source, ec);
        return;
    }
    std::filesystem::path kept = source;
    kept += L".damaged";
    std::filesystem::rename(source, kept, ec);
    QLOG_WARN(std::to_wstring(skippedPackets) + L" pacote(s) incompleto(s) foram pulados; original preservado em " + kept.wstring());
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

// Escolhe a janela a gravar: a que está em primeiro plano; se for o próprio Qlip, a de cima na ordem Z.
HWND findCaptureWindow(HWND exclude) {
    HWND foreground = GetForegroundWindow();
    if (isCapturableWindow(foreground, exclude)) return foreground;
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (isCapturableWindow(window, exclude)) return window;
    }
    return nullptr;
}

}  // namespace

RecordingManager::RecordingManager(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

RecordingManager::~RecordingManager() {
    if (recording_) stop();
    waitForIdle();
}

bool RecordingManager::start(const Options& options, std::wstring* error) {
    auto failStart = [error](const std::wstring& message) {
        if (error) *error = message;
        QLOG_ERROR(message);
        return false;
    };

    if (recording_) return failStart(L"Já existe uma gravação em andamento.");
    waitForIdle();  // uma finalização anterior pode ainda estar terminando

    replay_ = options.replayBuffer;
    if (!replay_) {
        std::error_code ec;
        std::filesystem::create_directories(options.folder, ec);
        if (ec) return failStart(L"Não foi possível criar a pasta de gravações: " + fromUtf8(ec.message()));
        if (!hasEnoughDiskSpace(options.folder, kStartMinFreeBytes)) {
            return failStart(L"Pouco espaço em disco para iniciar a gravação (mínimo 1 GB).");
        }

        const auto stamp = timestampName();
        tempFile_ = options.folder / (L"Qlip_" + stamp + std::wstring(kTempSuffix));
        finalFile_ = options.folder / (L"Qlip_" + stamp + L".mp4");
    }

    // Zero do tempo: vídeo e áudio medem a partir daqui, com o mesmo QPC.
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&frequency_));
    frameIntervalTicks_ = frequency_ / std::max(1, options.fps);
    startTicks_ = qpcNow();

    // Sem isso o Windows acorda threads só a cada ~15 ms, e o relógio CFR de 60 fps ficaria irregular.
    if (timeBeginPeriod(1) == TIMERR_NOERROR) timerResolutionRaised_ = true;

    av_log_set_callback(ffmpegLogCallback);
    writeFailed_ = false;
    dropped_ = 0;
    {
        // Não deixa o arquivo/erro/aviso da gravação anterior aparecer como se fosse desta.
        std::lock_guard lock(stateMutex_);
        lastError_.clear();
        lastWarning_.clear();
        lastFile_.clear();
    }
    frames_ = std::make_unique<BoundedQueue<Frame>>(kSlotCount);
    packets_ = std::make_unique<BoundedQueue<AVPacket*>>(kPacketQueue);

    // Alvo: o monitor escolhido (ou o principal) ou uma janela.
    HMONITOR monitor = findMonitor(options.monitorDevice);
    if (!monitor) {
        if (!options.monitorDevice.empty()) setWarning(L"O monitor escolhido não está conectado; gravando o principal.");
        monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    HWND window = nullptr;
    if (options.captureWindow) {
        window = findCaptureWindow(options.excludeWindow);
        if (!window) {
            cleanupAfterFailedStart();
            return failStart(L"Nenhuma janela encontrada para gravar. Abra o jogo ou escolha a área de trabalho nas configurações.");
        }
    }

    ScreenCapture::Callbacks captureCallbacks;
    captureCallbacks.onFrame = [this](const ScreenCapture::Frame& frame) { onCaptureFrame(frame); };
    captureCallbacks.onFatal = [this](const std::wstring& reason) { onCaptureFatal(reason); };

    std::wstring captureError;
    if (!capture_.start(monitor, window, options.fps, kSlotCount, std::move(captureCallbacks), &captureError)) {
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

    // Áudio: cada fonte é opcional. Falha de uma fonte vira aviso, e a gravação segue.
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
    if (!encoder_.open(encoderConfig, &encoderError)) {
        cleanupAfterFailedStart();
        return failStart(fromUtf8(encoderError));
    }

    if (replay_) {
        replay_->setCodecs(encoder_.codecContext(), wantAudio ? audioEncoder_.codecContext() : nullptr);
    } else {
        std::string muxError;
        if (!muxer_.open(tempFile_, encoder_.codecContext(), wantAudio ? audioEncoder_.codecContext() : nullptr, &muxError)) {
            cleanupAfterFailedStart();
            return failStart(fromUtf8(muxError));
        }
    }

    audioActive_ = wantAudio;
    producersLeft_ = wantAudio ? 2 : 1;
    stopFrame_ = -1;
    recording_ = true;
    encoderThread_ = std::thread([this] { encoderLoop(); });
    writerThread_ = std::thread([this] { writerLoop(); });
    if (wantAudio) audioThread_ = std::thread([this] { audioLoop(); });

    QLOG_INFO((replay_ ? std::wstring(L"Pipeline de replay iniciado: ") : std::wstring(L"Gravação iniciada: ")) +
              std::to_wstring(targetWidth) + L"x" + std::to_wstring(targetHeight) +
              L" @ " + std::to_wstring(options.fps) + L" fps (CFR), áudio: " +
              (wantAudio ? L"sim" : L"não") + (replay_ ? L"" : L" -> " + finalFile_.wstring()));
    return true;
}

void RecordingManager::cleanupAfterFailedStart() {
    capture_.stop();
    gameSource_.reset();
    micSource_.reset();
    encoder_.close();
    audioEncoder_.close();
    muxer_.close(nullptr);
    std::error_code ec;
    std::filesystem::remove(tempFile_, ec);
    frames_.reset();
    packets_.reset();
    audioActive_ = false;
    if (timerResolutionRaised_) {
        timeEndPeriod(1);
        timerResolutionRaised_ = false;
    }
}

void RecordingManager::stop() {
    if (!recording_.exchange(false)) return;
    stopFrame_ = (qpcNow() - startTicks_) * AudioEncoder::kSampleRate / frequency_;

    // Ordem importante: para a captura (nenhum frame novo entra) e fecha a fila de frames.
    // O encoder de vídeo para os ticks e faz flush; o áudio faz o último bloco e flush; o writer termina.
    capture_.stop();
    if (frames_) frames_->close();
    wake_.notify_all();

    finalizing_ = true;
    QLOG_INFO(L"Gravação parada; finalizando.");
    finalizer_ = std::thread([this] { finalize(); });
}

void RecordingManager::waitForIdle() {
    if (finalizer_.joinable()) finalizer_.join();
}

double RecordingManager::elapsedSeconds() const {
    if (!recording_) return 0.0;
    return static_cast<double>(qpcNow() - startTicks_) / static_cast<double>(frequency_);
}

std::filesystem::path RecordingManager::lastFile() const {
    std::lock_guard lock(stateMutex_);
    return lastFile_;
}

std::wstring RecordingManager::lastError() const {
    std::lock_guard lock(stateMutex_);
    return lastError_;
}

std::wstring RecordingManager::lastWarning() const {
    std::lock_guard lock(stateMutex_);
    return lastWarning_;
}

void RecordingManager::onCaptureFrame(const ScreenCapture::Frame& frame) {
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

void RecordingManager::onCaptureFatal(const std::wstring& reason) {
    fail(reason);
}

void RecordingManager::finishProducer() {
    // O último produtor a terminar fecha a fila: o writer só termina quando vídeo e áudio acabaram.
    if (producersLeft_.fetch_sub(1) == 1) packets_->close();
}

void RecordingManager::encoderLoop() {
    // Saída CFR: a cada tick de 1/FPS o encoder recebe o frame mais recente. Se a tela não mudou,
    // o último frame é repetido. O slot do frame segurado não volta ao pool até chegar outro.
    auto sink = [this](AVPacket* packet) {
        packet->stream_index = VideoMuxer::kVideoStream;
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
        if (writeFailed_) return;
        const int64_t pts = std::max<int64_t>(0, (tickTicks - startTicks_) * 1000000 / frequency_);
        bool ok = false;
        // viewSlot: lê sem liberar, porque o frame segurado será repetido nos próximos ticks.
        capture_.viewSlot(held.slot, [&](const uint8_t* data, int rowPitch) {
            ok = encoder_.encode(data, rowPitch, static_cast<int>(held.width), static_cast<int>(held.height), pts, sink);
        });
        if (!ok && !writeFailed_) fail(L"Falha ao codificar o vídeo com NVENC.");
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
    if (!writeFailed_ && !encoder_.flush(sink)) {
        fail(L"Falha ao finalizar a codificação do vídeo.");
    }
    finishProducer();
}

void RecordingManager::mixBlock(int64_t startFrame, float* mix, float* scratch) const {
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

void RecordingManager::audioLoop() {
    // Mixer: emite blocos de 1024 amostras no tempo da gravação, sempre com 100 ms de atraso.
    // Assim cada fonte já escreveu o que chegou no seu intervalo, e não há espera por dispositivo.
    std::vector<float> mix(AudioEncoder::kFrameSize * AudioRing::kChannels);
    std::vector<float> scratch(AudioEncoder::kFrameSize * AudioRing::kChannels);

    auto sink = [this](AVPacket* packet) {
        packet->stream_index = VideoMuxer::kAudioStream;
        AVPacket* copy = av_packet_clone(packet);
        if (!copy) return false;
        if (!packets_->push(copy)) {
            av_packet_free(&copy);
            return false;
        }
        return true;
    };

    auto emitBlock = [&](int64_t start) {
        if (writeFailed_) return;
        mixBlock(start, mix.data(), scratch.data());
        if (!audioEncoder_.encode(mix.data(), start, sink) && !writeFailed_) {
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
            // Gravando: mistura só o que já tem 100 ms de folga, para as fontes terem entregue os dados.
            while (next + kBlock <= nowFrame - kAudioMarginFrames) {
                emitBlock(next);
                next += kBlock;
            }
            waitTick();
            continue;
        }

        // Parando: espera 100 ms depois do stop (o WASAPI entrega com atraso), e então mistura até o instante do stop.
        // O último bloco é completado com silêncio, para não perder o fim da gravação.
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

    if (!writeFailed_ && !audioEncoder_.flush(sink)) fail(L"Falha ao finalizar o áudio.");
    finishProducer();
}

void RecordingManager::writerLoop() {
    AVPacket* packet = nullptr;
    int64_t written = 0;

    while (packets_->pop(packet)) {
        if (writeFailed_) {
            av_packet_free(&packet);
            continue;
        }

        if (replay_) {
            replay_->push(packet);  // o buffer assume a posse do pacote
            continue;
        }

        std::string error;
        // muxer_.write assume a posse do pacote, mesmo quando falha.
        if (!muxer_.write(packet, &error)) {
            fail(fromUtf8(error));
            continue;
        }

        if (++written % kDiskCheckEveryPackets == 0 && !hasEnoughDiskSpace(finalFile_.parent_path(), kStopMinFreeBytes)) {
            fail(L"Disco cheio: a gravação foi interrompida e salva até este ponto.");
        }
    }

    std::string closeError;
    if (!muxer_.close(&closeError) && !writeFailed_) {
        fail(fromUtf8(closeError));
    }
}

void RecordingManager::finalize() {
    if (encoderThread_.joinable()) encoderThread_.join();
    if (audioThread_.joinable()) audioThread_.join();
    if (writerThread_.joinable()) writerThread_.join();

    // Estatísticas da sessão: permitem medir perdas de frame sem depender da barra de status.
    // stopFrame_ é o instante do stop em amostras de 48 kHz (elapsedSeconds() já é 0 depois do stop).
    QLOG_INFO(L"Sessão encerrada: " + std::to_wstring(stopFrame_.load() / AudioEncoder::kSampleRate) + L" s, frames perdidos: " +
              std::to_wstring(dropped_.load()) + (replay_ ? L" (replay)" : L""));

    // Os mixers já terminaram: agora as fontes de áudio podem ser encerradas com segurança.
    gameSource_.reset();
    micSource_.reset();
    audioActive_ = false;

    if (timerResolutionRaised_) {
        timeEndPeriod(1);
        timerResolutionRaised_ = false;
    }

    std::error_code ec;
    if (replay_) {
        // Replay não tem arquivo: o buffer guarda os pacotes até o próximo start ou até ser limpo.
    } else if (std::filesystem::exists(tempFile_, ec)) {
        std::string error;
        int skipped = 0;
        if (VideoMuxer::remux(tempFile_, finalFile_, &error, &skipped)) {
            retireSource(tempFile_, skipped);
            std::lock_guard lock(stateMutex_);
            lastFile_ = finalFile_;
            QLOG_INFO(L"Gravação salva: " + finalFile_.wstring());
        } else {
            // O MKV temporário fica na pasta: será recuperado na próxima inicialização.
            setError(fromUtf8(error) + L" O arquivo temporário foi mantido para recuperação.");
        }
    } else {
        setError(L"Nenhum vídeo foi gravado.");
    }

    capture_.stop();
    encoder_.close();
    audioEncoder_.close();
    replay_ = nullptr;
    finalizing_ = false;
    if (callbacks_.onFinished) callbacks_.onFinished();
}

void RecordingManager::fail(const std::wstring& reason) {
    // Só a primeira falha vale; as seguintes são consequência (filas fechadas etc.).
    if (writeFailed_.exchange(true)) return;

    setError(reason);
    if (frames_) frames_->close();
    if (packets_) packets_->close();
    if (callbacks_.onFatal) callbacks_.onFatal(reason);
}

void RecordingManager::setError(const std::wstring& message) {
    std::lock_guard lock(stateMutex_);
    if (lastError_.empty()) lastError_ = message;
    QLOG_ERROR(message);
}

void RecordingManager::setWarning(const std::wstring& message) {
    std::lock_guard lock(stateMutex_);
    if (!lastWarning_.empty()) lastWarning_ += L" ";
    lastWarning_ += message;
    QLOG_WARN(message);
}

bool RecordingManager::hasEnoughDiskSpace(const std::filesystem::path& folder, uint64_t minBytes) const {
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(folder.c_str(), &freeBytes, nullptr, nullptr)) return true;  // sem informação: não bloqueia
    return freeBytes.QuadPart >= minBytes;
}

void RecordingManager::recoverLeftovers(const std::filesystem::path& folder) {
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec)) return;

    std::filesystem::directory_iterator it(folder, ec);
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        const std::wstring name = entry.path().filename().wstring();
        std::wstring_view suffix;
        for (std::wstring_view candidate : {kTempSuffix, kLegacyTempSuffix}) {
            if (name.size() > candidate.size() && name.compare(name.size() - candidate.size(), candidate.size(), candidate) == 0) {
                suffix = candidate;
            }
        }
        if (suffix.empty()) continue;

        const std::filesystem::path finalPath = entry.path().parent_path() / (name.substr(0, name.size() - suffix.size()) + L".mp4");
        QLOG_INFO(L"Recuperando gravação interrompida: " + entry.path().wstring());

        std::string error;
        int skipped = 0;
        if (VideoMuxer::remux(entry.path(), finalPath, &error, &skipped)) {
            // retireSource usa error_code próprio: o `ec` do laço não é afetado.
            retireSource(entry.path(), skipped);
            QLOG_INFO(L"Recuperação concluída: " + finalPath.wstring());
        } else {
            QLOG_ERROR(L"Não foi possível recuperar " + entry.path().wstring() + L": " + fromUtf8(error));
        }
    }
}
