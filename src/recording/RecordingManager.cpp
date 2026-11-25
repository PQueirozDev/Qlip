#include "recording/RecordingManager.h"

#include <string_view>

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

#include "encoder/AudioEncoder.h"
#include "utils/Logger.h"
#include "utils/Text.h"

namespace {

constexpr uint64_t kStartMinFreeBytes = 1024ull * 1024 * 1024;  // 1 GB para iniciar
constexpr uint64_t kStopMinFreeBytes = 256ull * 1024 * 1024;    // 256 MB durante a gravação
constexpr int kDiskCheckEveryPackets = 300;

int64_t qpcNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t qpcFrequency() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

std::wstring timestampName() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04d-%02d-%02d_%02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

// Arquivos temporários: Qlip_<data>.recording.mp4
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

}  // namespace

RecordingManager::RecordingManager(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

RecordingManager::~RecordingManager() {
    stop();
    waitForIdle();
}

bool RecordingManager::start(const std::shared_ptr<CapturePipeline>& pipeline, const std::filesystem::path& folder,
                             std::wstring* error) {
    auto failStart = [error](const std::wstring& message) {
        if (error) *error = message;
        QLOG_ERROR(message);
        return false;
    };

    {
        std::unique_lock lock(stateMutex_);
        if (recording_ || finalizing_) return failStart(L"Já existe uma gravação em andamento.");
        // A finalização anterior já terminou: o join é imediato.
        std::thread previous = std::move(finalizer_);
        lock.unlock();
        if (previous.joinable()) previous.join();
    }
    if (!pipeline || !pipeline->isRunning()) return failStart(L"A captura não está ativa.");

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) return failStart(L"Não foi possível criar a pasta de gravações: " + fromUtf8(ec.message()));
    if (!hasEnoughDiskSpace(folder, kStartMinFreeBytes)) {
        return failStart(L"Pouco espaço em disco para iniciar a gravação (mínimo 1 GB).");
    }

    const auto stamp = timestampName();
    tempFile_ = folder / (L"Qlip_" + stamp + std::wstring(kTempSuffix));
    finalFile_ = folder / (L"Qlip_" + stamp + L".mp4");

    std::string muxError;
    if (!muxer_.open(tempFile_, pipeline->videoCodec(), pipeline->audioCodec(), &muxError)) {
        std::filesystem::remove(tempFile_, ec);
        return failStart(fromUtf8(muxError));
    }

    // Estado da escrita: preparado antes de anexar (addSink sincroniza com a thread de escrita).
    hasAudio_ = pipeline->audioCodec() != nullptr;
    started_ = false;
    videoDone_ = false;
    audioDone_ = !hasAudio_;
    written_ = 0;
    done_ = false;
    writeFailed_ = false;
    endUs_ = INT64_MAX;
    startTicks_ = qpcNow();
    {
        std::lock_guard lock(stateMutex_);
        recording_ = true;
        finalizing_ = false;
        ended_ = false;
        lastError_.clear();
        lastFile_.clear();
        lastWarning_ = pipeline->warning();
    }
    pipeline_ = pipeline;

    // O pipeline pode estar rodando há tempo (replay): o arquivo começa num IDR pedido agora.
    pipeline->requestKeyframe();
    pipeline->addSink(this);
    QLOG_INFO(L"Gravação iniciada -> " + finalFile_.wstring());
    return true;
}

void RecordingManager::stop() {
    // O pipeline só é solto fora do lock: se esta for a última referência, ele espera a própria finalização,
    // que chama onEnd (que usa o lock).
    std::shared_ptr<CapturePipeline> pipeline = std::move(pipeline_);
    {
        std::lock_guard lock(stateMutex_);
        if (!recording_) return;
        recording_ = false;
        if (!ended_) finalizing_ = true;
        endUs_ = pipeline ? pipeline->nowUs() : 0;
    }
    QLOG_INFO(L"Gravação parada; finalizando.");
}

void RecordingManager::waitForIdle() {
    std::unique_lock lock(stateMutex_);
    idle_.wait(lock, [this] { return !finalizing_; });
    std::thread finalizer = std::move(finalizer_);
    lock.unlock();
    if (finalizer.joinable()) finalizer.join();
}

bool RecordingManager::isRecording() const {
    std::lock_guard lock(stateMutex_);
    return recording_;
}

bool RecordingManager::isFinalizing() const {
    std::lock_guard lock(stateMutex_);
    return finalizing_;
}

double RecordingManager::elapsedSeconds() const {
    if (!isRecording()) return 0.0;
    return static_cast<double>(qpcNow() - startTicks_) / static_cast<double>(qpcFrequency());
}

uint64_t RecordingManager::droppedFrames() const {
    return pipeline_ ? pipeline_->droppedFrames() : 0;
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

void RecordingManager::onPacket(const AVPacket* packet) {
    if (done_) return;
    const int64_t end = endUs_.load();

    if (packet->stream_index == CapturePipeline::kVideoStream) {
        if (videoDone_) return;
        if (packet->pts >= end) {
            videoDone_ = true;
        } else if (started_) {
            write(packet, baseVideoUs_);
        } else if (packet->flags & AV_PKT_FLAG_KEY) {
            // Primeiro IDR depois de anexar: é o zero do arquivo. Costuma ser o pedido no start; se um keyframe
            // periódico ainda estava no NVENC, o arquivo começa alguns frames antes, sem perder nada.
            started_ = true;
            baseVideoUs_ = packet->pts;
            baseAudioSamples_ = av_rescale(baseVideoUs_, AudioEncoder::kSampleRate, 1000000);
            write(packet, baseVideoUs_);
        }
    } else {
        if (audioDone_) return;
        // O áudio chega com a folga do mixer (~300 ms) de atraso: o fim só vale quando ele passa do stop.
        if (av_rescale(packet->pts, 1000000, AudioEncoder::kSampleRate) >= end) {
            audioDone_ = true;
        } else if (started_ && packet->pts >= baseAudioSamples_) {
            write(packet, baseAudioSamples_);
        }
    }

    if (videoDone_ && audioDone_) done_ = true;
}

void RecordingManager::write(const AVPacket* packet, int64_t offset) {
    if (writeFailed_) return;
    AVPacket* copy = av_packet_clone(packet);
    if (!copy) {
        failWrite(L"Memória insuficiente para gravar.");
        return;
    }
    copy->pts -= offset;
    if (copy->dts != AV_NOPTS_VALUE) copy->dts -= offset;

    std::string error;
    // muxer_.write assume a posse do pacote, mesmo quando falha.
    if (!muxer_.write(copy, &error)) {
        failWrite(fromUtf8(error));
        return;
    }
    if (++written_ % kDiskCheckEveryPackets == 0 && !hasEnoughDiskSpace(finalFile_.parent_path(), kStopMinFreeBytes)) {
        failWrite(L"Disco cheio: a gravação foi interrompida e salva até este ponto.");
    }
}

void RecordingManager::failWrite(const std::wstring& reason) {
    // Só a primeira falha vale. O destino termina (o pipeline continua, se houver replay) e salva o que já tem.
    if (writeFailed_.exchange(true)) return;
    done_ = true;
    setError(reason);
    if (callbacks_.onFatal) callbacks_.onFatal(reason);
}

void RecordingManager::onEnd() {
    std::lock_guard lock(stateMutex_);
    ended_ = true;
    recording_ = false;
    finalizing_ = true;
    if (finalizer_.joinable()) finalizer_.detach();  // não acontece: start() junta a finalização anterior
    finalizer_ = std::thread([this] { finalize(); });
}

void RecordingManager::finalize() {
    std::string closeError;
    const bool closed = muxer_.close(&closeError);

    std::error_code ec;
    if (!started_) {
        // Nenhum keyframe chegou (stop logo depois do start, ou falha do pipeline): não há vídeo para salvar.
        std::filesystem::remove(tempFile_, ec);
        setError(L"Nenhum vídeo foi gravado.");
    } else if (std::filesystem::exists(tempFile_, ec)) {
        if (!closed && !writeFailed_) setError(fromUtf8(closeError));
        std::string error;
        int skipped = 0;
        if (VideoMuxer::remux(tempFile_, finalFile_, &error, &skipped)) {
            retireSource(tempFile_, skipped);
            std::lock_guard lock(stateMutex_);
            lastFile_ = finalFile_;
            QLOG_INFO(L"Gravação salva: " + finalFile_.wstring());
        } else {
            // O temporário fica na pasta: será recuperado na próxima inicialização.
            setError(fromUtf8(error) + L" O arquivo temporário foi mantido para recuperação.");
        }
    } else {
        setError(L"Nenhum vídeo foi gravado.");
    }

    {
        std::lock_guard lock(stateMutex_);
        finalizing_ = false;
    }
    idle_.notify_all();
    if (callbacks_.onFinished) callbacks_.onFinished();
}

void RecordingManager::setError(const std::wstring& message) {
    std::lock_guard lock(stateMutex_);
    if (lastError_.empty()) lastError_ = message;
    QLOG_ERROR(message);
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
