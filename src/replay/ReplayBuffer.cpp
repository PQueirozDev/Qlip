#include "replay/ReplayBuffer.h"

#include <algorithm>

#include "utils/Text.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace {

constexpr size_t kNoIndex = static_cast<size_t>(-1);
constexpr int64_t kFolgaUs = 1000000;  // 1 s de folga além da duração pedida (para cortar no keyframe)

std::string errorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

AVCodecParameters* cloneParams(const AVCodecParameters* source) {
    if (!source) return nullptr;
    AVCodecParameters* copy = avcodec_parameters_alloc();
    if (copy && avcodec_parameters_copy(copy, source) < 0) avcodec_parameters_free(&copy);
    return copy;
}

}  // namespace

ReplayBuffer::Clip::~Clip() {
    for (Entry& entry : entries) {
        if (entry.packet) av_packet_free(&entry.packet);
    }
    avcodec_parameters_free(&video);
    avcodec_parameters_free(&audio);
}

ReplayBuffer::ReplayBuffer(size_t maxBytes) : maxBytes_(maxBytes) {}

ReplayBuffer::~ReplayBuffer() {
    clear();
    avcodec_parameters_free(&videoParams_);
    avcodec_parameters_free(&audioParams_);
}

void ReplayBuffer::setDurationSeconds(int seconds) {
    std::lock_guard lock(mutex_);
    durationSeconds_ = std::max(1, seconds);
}

void ReplayBuffer::setCodecs(const AVCodecContext* video, const AVCodecContext* audio) {
    std::lock_guard lock(mutex_);
    avcodec_parameters_free(&videoParams_);
    avcodec_parameters_free(&audioParams_);
    if (video) {
        videoParams_ = avcodec_parameters_alloc();
        if (videoParams_) avcodec_parameters_from_context(videoParams_, video);
    }
    if (audio) {
        audioParams_ = avcodec_parameters_alloc();
        if (audioParams_) avcodec_parameters_from_context(audioParams_, audio);
    }
}

void ReplayBuffer::push(AVPacket* packet) {
    Entry entry;
    entry.packet = packet;
    entry.video = packet->stream_index == kVideoStream;
    entry.keyframe = entry.video && (packet->flags & AV_PKT_FLAG_KEY) != 0;
    entry.bytes = static_cast<size_t>(packet->size);
    // Vídeo já está em microssegundos; áudio em amostras de 1/48000 s.
    entry.timeUs = entry.video ? packet->pts : packet->pts * 1000000 / kAudioSampleRate;

    std::lock_guard lock(mutex_);
    entries_.push_back(entry);
    bytes_ += entry.bytes;
    trimLocked();
}

void ReplayBuffer::clear() {
    std::lock_guard lock(mutex_);
    dropFront(entries_.size());
}

bool ReplayBuffer::snapshot(int seconds, Clip& clip) const {
    // Libera o conteúdo anterior do clipe (se houver) antes de preencher de novo.
    for (Entry& old : clip.entries) {
        if (old.packet) av_packet_free(&old.packet);
    }
    clip.entries.clear();
    avcodec_parameters_free(&clip.video);
    avcodec_parameters_free(&clip.audio);

    std::lock_guard lock(mutex_);
    if (entries_.empty()) return false;

    // Início: o último keyframe de vídeo em (agora - seconds). Se o buffer for mais curto, o primeiro keyframe.
    const int64_t newest = entries_.back().timeUs;
    const int64_t cutoff = newest - static_cast<int64_t>(std::max(1, seconds)) * 1000000;
    size_t start = kNoIndex;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (!e.video || !e.keyframe) continue;
        if (e.timeUs <= cutoff) start = i;
        else if (start == kNoIndex) start = i;
        if (e.timeUs > cutoff) break;
    }
    if (start == kNoIndex) return false;

    for (size_t i = start; i < entries_.size(); ++i) {
        Entry copy = entries_[i];
        copy.packet = av_packet_clone(entries_[i].packet);
        if (!copy.packet) {
            // Falhou no meio: libera o que já foi clonado antes de sair.
            for (Entry& partial : clip.entries) av_packet_free(&partial.packet);
            clip.entries.clear();
            return false;
        }
        clip.entries.push_back(copy);
    }

    // O áudio chega com a folga do mixer de atraso: sem cortar, o fim do clipe teria vídeo sem som.
    // Termina o clipe no último pacote de áudio (se houver áudio no buffer).
    int64_t lastAudio = -1;
    for (const Entry& e : clip.entries) {
        if (!e.video) lastAudio = std::max(lastAudio, e.timeUs);
    }
    // Só corta se o som cobre o início do clipe; senão sobraria um clipe sem vídeo.
    if (lastAudio >= 0 && !clip.entries.empty() && lastAudio >= clip.entries.front().timeUs) {
        // O último pacote de áudio cobre 1024 amostras (~21 ms) a partir do seu início.
        const int64_t audioEnd = lastAudio + 1024 * 1000000 / kAudioSampleRate;
        // O vídeo chega antes do áudio, então os quadros depois do fim do som ficam espalhados no fim da lista.
        auto beyond = std::remove_if(clip.entries.begin(), clip.entries.end(), [audioEnd](Entry& e) {
            if (!e.video || e.timeUs <= audioEnd) return false;
            av_packet_free(&e.packet);
            return true;
        });
        clip.entries.erase(beyond, clip.entries.end());
    }
    clip.video = cloneParams(videoParams_);
    clip.audio = cloneParams(audioParams_);
    return clip.video != nullptr;
}

double ReplayBuffer::bufferedSeconds() const {
    std::lock_guard lock(mutex_);
    if (entries_.size() < 2) return 0.0;
    return static_cast<double>(entries_.back().timeUs - entries_.front().timeUs) / 1000000.0;
}

size_t ReplayBuffer::bytes() const {
    std::lock_guard lock(mutex_);
    return bytes_;
}

void ReplayBuffer::dropFront(size_t count) {
    for (size_t i = 0; i < count && i < entries_.size(); ++i) {
        bytes_ -= entries_[i].bytes;
        av_packet_free(&entries_[i].packet);
    }
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(std::min(count, entries_.size())));
}

void ReplayBuffer::trimLocked() {
    if (entries_.empty()) return;

    // Tempo: descarta tudo antes do keyframe que fica logo antes de (agora - duração - folga).
    const int64_t cutoff = entries_.back().timeUs - (static_cast<int64_t>(durationSeconds_) * 1000000 + kFolgaUs);
    size_t keep = kNoIndex;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.timeUs > cutoff) break;
        if (e.video && e.keyframe) keep = i;
    }
    if (keep != kNoIndex && keep > 0) dropFront(keep);

    // Memória: remove GOPs inteiros do início até voltar ao teto.
    // Sem keyframe à frente, o teto é cumprido descartando tudo: o vídeo só volta no próximo keyframe.
    while (bytes_ > maxBytes_ && !entries_.empty()) {
        size_t next = kNoIndex;
        for (size_t i = 1; i < entries_.size(); ++i) {
            if (entries_[i].video && entries_[i].keyframe) {
                next = i;
                break;
            }
        }
        dropFront(next == kNoIndex ? entries_.size() : next);
    }
}

namespace clip {

bool save(const ReplayBuffer::Clip& source, const std::filesystem::path& file, std::string* error) {
    auto fail = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (source.entries.empty() || !source.video) return fail("Não há vídeo para salvar no clipe.");

    // O clipe começa no keyframe que abre o buffer copiado; os tempos são reescritos para começar em zero.
    const int64_t startUs = source.entries.front().timeUs;
    const int64_t startAudioSamples = startUs * ReplayBuffer::kAudioSampleRate / 1000000;

    const std::filesystem::path tempPath = file.wstring() + L".tmp";
    const std::string tmpPath = toUtf8(tempPath.wstring());

    AVFormatContext* output = nullptr;
    int result = avformat_alloc_output_context2(&output, nullptr, "mp4", tmpPath.c_str());
    if (result < 0 || !output) return fail("Falha ao criar o clipe (" + errorText(result) + ").");

    auto cleanup = [&] {
        if (output) {
            if (output->pb) avio_closep(&output->pb);
            avformat_free_context(output);
            output = nullptr;
        }
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    };

    AVStream* videoStream = avformat_new_stream(output, nullptr);
    if (!videoStream || avcodec_parameters_copy(videoStream->codecpar, source.video) < 0) {
        cleanup();
        return fail("Falha ao configurar o vídeo do clipe.");
    }
    videoStream->codecpar->codec_tag = 0;
    videoStream->codecpar->video_delay = 0;

    AVStream* audioStream = nullptr;
    if (source.audio) {
        audioStream = avformat_new_stream(output, nullptr);
        if (!audioStream || avcodec_parameters_copy(audioStream->codecpar, source.audio) < 0) {
            cleanup();
            return fail("Falha ao configurar o áudio do clipe.");
        }
        audioStream->codecpar->codec_tag = 0;
    }

    result = avio_open(&output->pb, tmpPath.c_str(), AVIO_FLAG_WRITE);
    if (result < 0) {
        cleanup();
        return fail("Não foi possível criar o arquivo do clipe (" + errorText(result) + ").");
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0);
    result = avformat_write_header(output, &options);
    av_dict_free(&options);
    if (result < 0) {
        cleanup();
        return fail("Falha ao escrever o cabeçalho do clipe (" + errorText(result) + ").");
    }

    for (const ReplayBuffer::Entry& entry : source.entries) {
        AVStream* stream = entry.video ? videoStream : audioStream;
        if (!stream || !entry.packet) continue;

        // Cópia: o Clip continua intacto (ele é liberado por inteiro depois).
        AVPacket* packet = av_packet_clone(entry.packet);
        if (!packet) {
            cleanup();
            return fail("Memória insuficiente para salvar o clipe.");
        }

        AVRational inBase{1, 1000000};
        if (entry.video) {
            packet->pts -= startUs;
        } else {
            inBase = AVRational{1, ReplayBuffer::kAudioSampleRate};
            packet->pts -= startAudioSamples;
        }
        if (packet->pts < 0) {  // áudio que termina antes do keyframe: não faz parte do clipe
            av_packet_free(&packet);
            continue;
        }
        packet->dts = packet->pts;
        av_packet_rescale_ts(packet, inBase, stream->time_base);
        packet->stream_index = stream->index;

        const int writeResult = av_interleaved_write_frame(output, packet);
        av_packet_free(&packet);
        if (writeResult < 0) {
            cleanup();
            return fail("Falha ao gravar o clipe (" + errorText(writeResult) + ").");
        }
    }

    result = av_write_trailer(output);
    if (result < 0) {
        cleanup();
        return fail("Falha ao finalizar o clipe (" + errorText(result) + ").");
    }
    const int closeResult = avio_closep(&output->pb);
    avformat_free_context(output);
    output = nullptr;
    if (closeResult < 0) {
        cleanup();
        return fail("Falha ao salvar o clipe no disco (" + errorText(closeResult) + ").");
    }

    std::error_code ec;
    std::filesystem::rename(tempPath, file, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return fail("Não foi possível salvar o clipe: " + ec.message());
    }
    return true;
}

}  // namespace clip
