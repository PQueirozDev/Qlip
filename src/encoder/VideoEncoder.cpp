#include "encoder/VideoEncoder.h"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

std::string errorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

}  // namespace

VideoEncoder::~VideoEncoder() {
    close();
}

bool VideoEncoder::open(const Config& config, std::string* error) {
    close();
    hasKeyframe_ = false;

    auto fail = [this, error](std::string message) {
        if (error) *error = std::move(message);
        close();
        return false;
    };

    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) return fail("O FFmpeg usado não tem o encoder NVENC (h264_nvenc).");

    codec_ = avcodec_alloc_context3(codec);
    if (!codec_) return fail("Falha ao criar o contexto do encoder.");

    codec_->width = config.width;
    codec_->height = config.height;
    codec_->pix_fmt = AV_PIX_FMT_NV12;
    codec_->time_base = AVRational{1, 1000000};  // timestamps em microssegundos
    codec_->framerate = AVRational{config.fps, 1};
    codec_->bit_rate = static_cast<int64_t>(config.bitrateKbps) * 1000;
    codec_->rc_max_rate = codec_->bit_rate;
    codec_->rc_buffer_size = static_cast<int>(codec_->bit_rate);
    codec_->gop_size = config.fps;  // keyframe a cada 1 s: base para o replay cortar em keyframe
    codec_->max_b_frames = 0;       // sem B-frames: menor latência e uso de GPU
    // Cabeçalhos SPS/PPS no contexto: o muxer MP4 precisa deles.
    codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // p2 + ll: encoder rápido, com baixa latência e pouco impacto no jogo.
    av_opt_set(codec_->priv_data, "preset", "p2", 0);
    av_opt_set(codec_->priv_data, "tune", "ll", 0);
    av_opt_set(codec_->priv_data, "rc", "vbr", 0);

    const int openResult = avcodec_open2(codec_, codec, nullptr);
    if (openResult < 0) {
        return fail("NVENC indisponível ou ocupado (" + errorText(openResult) + "). Verifique o driver NVIDIA e se outro app está usando o encoder.");
    }

    frame_ = av_frame_alloc();
    if (!frame_) return fail("Memória insuficiente para o frame de vídeo.");
    frame_->format = AV_PIX_FMT_NV12;
    frame_->width = config.width;
    frame_->height = config.height;
    if (av_frame_get_buffer(frame_, 32) < 0) return fail("Memória insuficiente para os buffers de vídeo.");

    return true;
}

void VideoEncoder::close() {
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (frame_) av_frame_free(&frame_);
    if (codec_) avcodec_free_context(&codec_);
}

bool VideoEncoder::encode(const uint8_t* bgra, int srcStride, int srcWidth, int srcHeight, int64_t ptsUs, const PacketSink& sink) {
    if (!codec_ || !frame_) return false;

    // Reconfigura a conversão quando a resolução de origem muda (ex.: troca de monitor).
    sws_ = sws_getCachedContext(sws_, srcWidth, srcHeight, AV_PIX_FMT_BGRA,
                                codec_->width, codec_->height, AV_PIX_FMT_NV12,
                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return false;
    srcWidth_ = srcWidth;
    srcHeight_ = srcHeight;

    if (av_frame_make_writable(frame_) < 0) return false;

    const uint8_t* srcData[4] = {bgra, nullptr, nullptr, nullptr};
    const int srcLinesize[4] = {srcStride, 0, 0, 0};
    sws_scale(sws_, srcData, srcLinesize, 0, srcHeight_, frame_->data, frame_->linesize);

    // Keyframe a cada 1 s de tempo real. O gop_size conta frames, e com captura VFR
    // (WGC só entrega frame quando a tela muda) isso faria os keyframes caírem em tempos irregulares.
    const bool keyframe = !hasKeyframe_ || ptsUs - lastKeyframePts_ >= 1000000;
    if (keyframe) {
        frame_->pict_type = AV_PICTURE_TYPE_I;
        lastKeyframePts_ = ptsUs;
        hasKeyframe_ = true;
    } else {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    frame_->pts = ptsUs;
    const int sendResult = avcodec_send_frame(codec_, frame_);
    if (sendResult < 0) return false;

    return drain(sink);
}

bool VideoEncoder::flush(const PacketSink& sink) {
    if (!codec_) return false;
    if (avcodec_send_frame(codec_, nullptr) < 0) return false;
    return drain(sink);
}

bool VideoEncoder::drain(const PacketSink& sink) {
    AVPacket* packet = av_packet_alloc();
    if (!packet) return false;

    bool ok = true;
    for (;;) {
        const int result = avcodec_receive_packet(codec_, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) {
            ok = false;
            break;
        }
        // Sem B-frames a ordem de decodificação é a de apresentação: dts == pts.
        // Isso evita dts irregular do encoder com captura VFR (o muxer rejeitava alguns pacotes no início).
        if (packet->pts != AV_NOPTS_VALUE) packet->dts = packet->pts;
        const bool keepGoing = sink(packet);
        av_packet_unref(packet);
        if (!keepGoing) {
            ok = false;
            break;
        }
    }

    av_packet_free(&packet);
    return ok;
}
