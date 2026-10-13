#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

// Buffer circular de áudio estéreo float indexado por amostra absoluta (desde o início da gravação).
// Cada fonte escreve no índice correspondente ao seu tempo real (via QPC), e o mixer lê o mesmo
// intervalo de tempo de todas as fontes. Por isso vídeo e áudio ficam sincronizados por construção.
// Trechos sem dados (ex.: loopback sem som) são lidos como silêncio.
class AudioRing {
public:
    static constexpr int kChannels = 2;
    static constexpr int64_t kCapacityFrames = 48000 * 4;  // 4 s de folga para o mixer

    // Escreve frames a partir de startFrame. Gaps entre o fim anterior e startFrame viram silêncio.
    void write(int64_t startFrame, const float* interleaved, int frames) {
        std::lock_guard lock(mutex_);
        if (frames <= 0 || startFrame < 0) return;
        if (startFrame < end_ - kCapacityFrames) return;  // atrasado demais: já foi sobrescrito

        if (startFrame > end_) {
            // Limpa o intervalo vazio para não ler dados antigos do anel.
            const int64_t gap = std::min(startFrame - end_, kCapacityFrames);
            for (int64_t i = 0; i < gap; ++i) {
                const int64_t idx = (startFrame - 1 - i) % kCapacityFrames;
                std::fill_n(&buffer_[static_cast<size_t>(idx) * kChannels], kChannels, 0.0f);
            }
        }

        for (int i = 0; i < frames; ++i) {
            const int64_t idx = (startFrame + i) % kCapacityFrames;
            for (int c = 0; c < kChannels; ++c) {
                buffer_[static_cast<size_t>(idx) * kChannels + c] = interleaved[i * kChannels + c];
            }
        }
        if (startFrame + frames > end_) end_ = startFrame + frames;
    }

    // Esvazia o anel: uma nova gravação recomeça o tempo em zero.
    void reset() {
        std::lock_guard lock(mutex_);
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        end_ = 0;
    }

    // Lê frames a partir de startFrame; o que não foi escrito ainda (ou já saiu do anel) vira silêncio.
    void read(int64_t startFrame, float* out, int frames) const {
        std::lock_guard lock(mutex_);
        const int64_t oldest = end_ - kCapacityFrames;
        for (int i = 0; i < frames; ++i) {
            const int64_t idx = startFrame + i;
            float* dst = &out[i * kChannels];
            if (idx < 0 || idx >= end_ || idx < oldest) {
                dst[0] = dst[1] = 0.0f;
                continue;
            }
            const size_t pos = static_cast<size_t>(idx % kCapacityFrames) * kChannels;
            dst[0] = buffer_[pos];
            dst[1] = buffer_[pos + 1];
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<float> buffer_ = std::vector<float>(static_cast<size_t>(kCapacityFrames) * kChannels, 0.0f);
    int64_t end_ = 0;  // uma posição depois da última amostra escrita
};
