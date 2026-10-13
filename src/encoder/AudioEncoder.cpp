#include "encoder/AudioEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace {

std::string errorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

}  // namespace

AudioEncoder::~AudioEncoder() {
    close();
}

bool AudioEncoder::open(int bitrateKbps, std::string* error) {
    close();
    auto fail = [this, error](std::string message) {
        if (error) *error = std::move(message);
        close();
        return false;
    };

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return fail("O FFmpeg usado não tem o encoder AAC.");

    codec_ = avcodec_alloc_context3(codec);
    if (!codec_) return fail("Falha ao criar o contexto do encoder de áudio.");

    codec_->sample_rate = kSampleRate;
    codec_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&codec_->ch_layout, 2);
    codec_->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
    codec_->time_base = AVRational{1, kSampleRate};
    codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    const int openResult = avcodec_open2(codec_, codec, nullptr);
    if (openResult < 0) return fail("Falha ao abrir o encoder de áudio (" + errorText(openResult) + ").");

    frame_ = av_frame_alloc();
    if (!frame_) return fail("Memória insuficiente para o áudio.");
    frame_->format = AV_SAMPLE_FMT_FLTP;
    frame_->nb_samples = kFrameSize;
    av_channel_layout_copy(&frame_->ch_layout, &codec_->ch_layout);
    if (av_frame_get_buffer(frame_, 0) < 0) return fail("Memória insuficiente para os buffers de áudio.");

    return true;
}

void AudioEncoder::close() {
    if (frame_) av_frame_free(&frame_);
    if (codec_) avcodec_free_context(&codec_);
}

bool AudioEncoder::encode(const float* interleaved, int64_t startSample, const PacketSink& sink) {
    if (!codec_ || !frame_) return false;
    if (av_frame_make_writable(frame_) < 0) return false;

    // AAC espera planar (L e R em planos separados).
    auto* left = reinterpret_cast<float*>(frame_->data[0]);
    auto* right = reinterpret_cast<float*>(frame_->data[1]);
    for (int i = 0; i < kFrameSize; ++i) {
        left[i] = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
    }

    frame_->pts = startSample;
    if (avcodec_send_frame(codec_, frame_) < 0) return false;
    return drain(sink);
}

bool AudioEncoder::flush(const PacketSink& sink) {
    if (!codec_) return false;
    if (avcodec_send_frame(codec_, nullptr) < 0) return false;
    return drain(sink);
}

bool AudioEncoder::drain(const PacketSink& sink) {
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
