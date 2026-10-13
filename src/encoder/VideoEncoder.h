#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// Encoder H.264 por hardware (NVENC, via h264_nvenc do libavcodec).
// Recebe BGRA da captura, converte para NV12 com escala e entrega pacotes codificados.
class VideoEncoder {
public:
    struct Config {
        int width = 1920;
        int height = 1080;
        int fps = 60;
        int bitrateKbps = 12000;
    };

    // Recebe cada pacote pronto. Retorna false para interromper a codificação.
    using PacketSink = std::function<bool(AVPacket*)>;

    VideoEncoder() = default;
    ~VideoEncoder();

    bool open(const Config& config, std::string* error);
    void close();

    // Codifica um frame BGRA. ptsUs é o tempo do frame em microssegundos desde o início.
    bool encode(const uint8_t* bgra, int srcStride, int srcWidth, int srcHeight, int64_t ptsUs, const PacketSink& sink);
    // Envia o restante dos pacotes que o encoder ainda guarda.
    bool flush(const PacketSink& sink);

    // Parâmetros usados pelo muxer (SPS/PPS e time base).
    const AVCodecContext* codecContext() const { return codec_; }

private:
    bool drain(const PacketSink& sink);

    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* sws_ = nullptr;
    int srcWidth_ = 0;
    int srcHeight_ = 0;
    int64_t lastKeyframePts_ = 0;
    bool hasKeyframe_ = false;
};
