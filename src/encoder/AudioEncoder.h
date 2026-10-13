#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// Encoder AAC (nativo do FFmpeg, LGPL). Recebe blocos de 1024 amostras estéreo float intercaladas.
class AudioEncoder {
public:
    static constexpr int kFrameSize = 1024;
    static constexpr int kSampleRate = 48000;

    using PacketSink = std::function<bool(AVPacket*)>;

    AudioEncoder() = default;
    ~AudioEncoder();

    bool open(int bitrateKbps, std::string* error);
    void close();

    // samples: kFrameSize * 2 floats intercalados. startSample: posição no tempo da gravação (1/48000 s).
    bool encode(const float* interleaved, int64_t startSample, const PacketSink& sink);
    bool flush(const PacketSink& sink);

    const AVCodecContext* codecContext() const { return codec_; }

private:
    bool drain(const PacketSink& sink);

    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
};
