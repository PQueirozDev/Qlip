#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;

// Buffer circular de pacotes já codificados (H.264 e AAC), com tempo em microssegundos.
// Nunca guarda frames brutos: a RAM usada é só o bitrate do encoder vezes a duração.
// Mantém a duração configurada (+1 s de folga) e um teto de memória; sempre corta em keyframe.
class ReplayBuffer {
public:
    static constexpr int kVideoStream = 0;
    static constexpr int kAudioStream = 1;
    static constexpr int kAudioSampleRate = 48000;

    struct Entry {
        AVPacket* packet = nullptr;  // dono do pacote
        int64_t timeUs = 0;          // tempo de apresentação, em microssegundos desde o início da gravação
        bool video = false;
        bool keyframe = false;
        size_t bytes = 0;
    };

    // Clipe pronto para gravar: pacotes clonados e cópias dos parâmetros dos codecs. Não copiável.
    struct Clip {
        std::vector<Entry> entries;
        AVCodecParameters* video = nullptr;
        AVCodecParameters* audio = nullptr;

        Clip() = default;
        Clip(const Clip&) = delete;
        Clip& operator=(const Clip&) = delete;
        ~Clip();
    };

    explicit ReplayBuffer(size_t maxBytes = 512ull * 1024 * 1024);
    ~ReplayBuffer();

    void setDurationSeconds(int seconds);
    // Guarda cópias dos parâmetros dos codecs (usados ao gravar o clipe).
    void setCodecs(const AVCodecContext* video, const AVCodecContext* audio);
    // Assume a posse do pacote.
    void push(AVPacket* packet);
    void clear();

    // Clipe com os últimos `seconds` (ou o máximo disponível), começando em keyframe.
    bool snapshot(int seconds, Clip& clip) const;

    double bufferedSeconds() const;
    size_t bytes() const;

private:
    void dropFront(size_t count);
    void trimLocked();

    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
    size_t bytes_ = 0;
    size_t maxBytes_;
    int durationSeconds_ = 30;
    AVCodecParameters* videoParams_ = nullptr;
    AVCodecParameters* audioParams_ = nullptr;
};

namespace clip {

// Grava o clipe em MP4 (com faststart), reescrevendo os tempos para começar em zero.
// Escreve em arquivo temporário e renomeia no fim, então um clipe incompleto nunca aparece com o nome final.
bool save(const ReplayBuffer::Clip& clip, const std::filesystem::path& file, std::string* error);

}  // namespace clip
