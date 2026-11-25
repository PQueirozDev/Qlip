#include "encoder/VideoEncoder.h"

#include <algorithm>

#include <d3d11_1.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

namespace {

using Microsoft::WRL::ComPtr;

// Limite das caches de views: se a captura recriar muitas texturas (resizes seguidos), recomeça do zero.
constexpr size_t kMaxCachedViews = 32;

std::string errorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

}  // namespace

VideoEncoder::~VideoEncoder() {
    close();
}

bool VideoEncoder::open(const Config& config, ID3D11Device* device, std::string* error) {
    close();
    hasKeyframe_ = false;
    keyframeRequested_ = false;
    width_ = config.width;
    height_ = config.height;

    auto fail = [this, error](std::string message) {
        if (error) *error = std::move(message);
        close();
        return false;
    };

    if (!device) return fail("Dispositivo de vídeo indisponível.");
    device_ = device;
    ComPtr<ID3D11DeviceContext> context;
    device_->GetImmediateContext(&context);
    if (FAILED(device_.As(&videoDevice_)) || FAILED(context.As(&videoContext_))) {
        return fail("A GPU não oferece o processador de vídeo do Direct3D 11.");
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) return fail("O FFmpeg usado não tem o encoder NVENC (h264_nvenc).");

    // Dispositivo e frames D3D11 do FFmpeg sobre o mesmo dispositivo da captura: o NVENC lê as texturas direto.
    hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwDevice_) return fail("Falha ao criar o contexto de vídeo da GPU.");
    auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(hwDevice_->data);
    auto* d3dDevice = static_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
    d3dDevice->device = device;
    device->AddRef();  // o FFmpeg libera a referência dele ao destruir o contexto
    int result = av_hwdevice_ctx_init(hwDevice_);
    if (result < 0) return fail("Falha ao iniciar o contexto de vídeo da GPU (" + errorText(result) + ").");

    hwFrames_ = av_hwframe_ctx_alloc(hwDevice_);
    if (!hwFrames_) return fail("Falha ao criar os buffers de vídeo da GPU.");
    auto* frames = reinterpret_cast<AVHWFramesContext*>(hwFrames_->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = config.width;
    frames->height = config.height;
    // Pool dinâmico (texturas avulsas, reaproveitadas): array de NV12 com render target não é aceito pelos drivers.
    // Ficam vivas só as texturas em voo (a convertida agora e as que o NVENC ainda codifica).
    frames->initial_pool_size = 0;
    // Render target: saída do processador de vídeo (conversão BGRA -> NV12).
    static_cast<AVD3D11VAFramesContext*>(frames->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    result = av_hwframe_ctx_init(hwFrames_);
    if (result < 0) return fail("Memória de GPU insuficiente para os buffers de vídeo (" + errorText(result) + ").");

    codec_ = avcodec_alloc_context3(codec);
    if (!codec_) return fail("Falha ao criar o contexto do encoder.");

    codec_->width = config.width;
    codec_->height = config.height;
    codec_->pix_fmt = AV_PIX_FMT_D3D11;
    codec_->hw_frames_ctx = av_buffer_ref(hwFrames_);
    if (!codec_->hw_frames_ctx) return fail("Memória insuficiente para o encoder.");
    codec_->time_base = AVRational{1, 1000000};  // timestamps em microssegundos
    codec_->framerate = AVRational{config.fps, 1};
    codec_->bit_rate = static_cast<int64_t>(config.bitrateKbps) * 1000;
    codec_->rc_max_rate = codec_->bit_rate;
    codec_->rc_buffer_size = static_cast<int>(codec_->bit_rate);
    codec_->gop_size = config.fps;  // keyframe a cada 1 s: base para o replay cortar em keyframe
    codec_->max_b_frames = 0;       // sem B-frames: menor latência e uso de GPU
    // A conversão de cor gera BT.709 com faixa limitada: o arquivo declara isso, e os players mostram as cores certas.
    codec_->color_range = AVCOL_RANGE_MPEG;
    codec_->colorspace = AVCOL_SPC_BT709;
    codec_->color_primaries = AVCOL_PRI_BT709;
    codec_->color_trc = AVCOL_TRC_BT709;
    // Cabeçalhos SPS/PPS no contexto: o muxer MP4 precisa deles.
    codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // p2 + ll: encoder rápido, com baixa latência e pouco impacto no jogo.
    av_opt_set(codec_->priv_data, "preset", "p2", 0);
    av_opt_set(codec_->priv_data, "tune", "ll", 0);
    av_opt_set(codec_->priv_data, "rc", "vbr", 0);
    // Keyframe pedido = IDR de verdade: é onde o replay e as gravações podem começar.
    av_opt_set_int(codec_->priv_data, "forced-idr", 1, 0);

    result = avcodec_open2(codec_, codec, nullptr);
    if (result < 0) {
        return fail("NVENC indisponível ou ocupado (" + errorText(result) + "). Verifique o driver NVIDIA e se outro app está usando o encoder.");
    }

    frame_ = av_frame_alloc();
    if (!frame_) return fail("Memória insuficiente para o frame de vídeo.");
    return true;
}

void VideoEncoder::close() {
    if (frame_) av_frame_free(&frame_);
    frameReady_ = false;
    if (codec_) avcodec_free_context(&codec_);
    inputViews_.clear();
    outputViews_.clear();
    processor_.Reset();
    enumerator_.Reset();
    processorSrcWidth_ = 0;
    processorSrcHeight_ = 0;
    if (hwFrames_) av_buffer_unref(&hwFrames_);
    if (hwDevice_) av_buffer_unref(&hwDevice_);
    videoContext_.Reset();
    videoDevice_.Reset();
    device_.Reset();
}

bool VideoEncoder::ensureProcessor(int srcWidth, int srcHeight) {
    if (processor_ && processorSrcWidth_ == srcWidth && processorSrcHeight_ == srcHeight) return true;

    // Origem mudou de tamanho (troca de resolução, janela redimensionada): as views são do processador antigo.
    inputViews_.clear();
    outputViews_.clear();
    processor_.Reset();
    enumerator_.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = static_cast<UINT>(srcWidth);
    desc.InputHeight = static_cast<UINT>(srcHeight);
    desc.OutputWidth = static_cast<UINT>(width_);
    desc.OutputHeight = static_cast<UINT>(height_);
    desc.InputFrameRate = DXGI_RATIONAL{static_cast<UINT>(codec_->framerate.num), 1};
    desc.OutputFrameRate = desc.InputFrameRate;
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&desc, &enumerator_))) return false;

    UINT support = 0;
    if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        return false;
    }
    if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        return false;
    }
    if (FAILED(videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) return false;

    ID3D11VideoProcessor* processor = processor_.Get();
    videoContext_->VideoProcessorSetStreamFrameFormat(processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    // Sem "melhorias" automáticas do driver (nitidez, redução de ruído): a imagem sai como foi capturada.
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);
    const RECT source{0, 0, srcWidth, srcHeight};
    const RECT target{0, 0, width_, height_};
    videoContext_->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &source);
    videoContext_->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &target);
    videoContext_->VideoProcessorSetOutputTargetRect(processor, TRUE, &target);

    // RGB da tela (faixa completa) -> YUV BT.709 com faixa limitada (o padrão dos players para vídeo HD).
    ComPtr<ID3D11VideoContext1> videoContext1;
    if (SUCCEEDED(videoContext_.As(&videoContext1))) {
        videoContext1->VideoProcessorSetStreamColorSpace1(processor, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        videoContext1->VideoProcessorSetOutputColorSpace1(processor, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{};
        input.RGB_Range = 0;      // 0-255
        input.YCbCr_Matrix = 1;   // BT.709
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
        output.YCbCr_Matrix = 1;
        output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext_->VideoProcessorSetStreamColorSpace(processor, 0, &input);
        videoContext_->VideoProcessorSetOutputColorSpace(processor, &output);
    }

    processorSrcWidth_ = srcWidth;
    processorSrcHeight_ = srcHeight;
    return true;
}

ID3D11VideoProcessorOutputView* VideoEncoder::outputView(ID3D11Texture2D* texture, UINT arraySlice) {
    // Chave: textura + fatia. A view segura a textura, então o ponteiro não é reaproveitado enquanto está na cache.
    const UINT key = arraySlice;
    auto found = outputViews_.find(key);
    if (found != outputViews_.end()) {
        ComPtr<ID3D11Resource> resource;
        found->second->GetResource(&resource);
        if (resource.Get() == static_cast<ID3D11Resource*>(texture)) return found->second.Get();
        outputViews_.erase(found);
    }
    if (outputViews_.size() >= kMaxCachedViews) outputViews_.clear();

    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
    if (textureDesc.ArraySize > 1) {
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.MipSlice = 0;
        desc.Texture2DArray.FirstArraySlice = arraySlice;
        desc.Texture2DArray.ArraySize = 1;
    } else {
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
    }
    ComPtr<ID3D11VideoProcessorOutputView> view;
    if (FAILED(videoDevice_->CreateVideoProcessorOutputView(texture, enumerator_.Get(), &desc, &view))) return nullptr;
    outputViews_[key] = view;
    return view.Get();
}

bool VideoEncoder::convert(ID3D11Texture2D* bgra, int srcWidth, int srcHeight) {
    if (!codec_ || !frame_ || !bgra || srcWidth <= 0 || srcHeight <= 0) return false;
    av_frame_unref(frame_);
    frameReady_ = false;
    if (!ensureProcessor(srcWidth, srcHeight)) return false;

    // Textura NV12 livre do pool do FFmpeg: data[0] = textura, data[1] = fatia do array.
    if (av_hwframe_get_buffer(hwFrames_, frame_, 0) < 0) return false;
    auto* target = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
    const auto slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame_->data[1]));

    ID3D11VideoProcessorInputView* input = nullptr;
    for (const InputView& cached : inputViews_) {
        if (cached.texture.Get() == bgra) input = cached.view.Get();
    }
    if (!input) {
        if (inputViews_.size() >= kMaxCachedViews) inputViews_.clear();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        InputView created;
        created.texture = bgra;
        if (FAILED(videoDevice_->CreateVideoProcessorInputView(bgra, enumerator_.Get(), &desc, &created.view))) return false;
        input = created.view.Get();
        inputViews_.push_back(std::move(created));
    }

    ID3D11VideoProcessorOutputView* output = outputView(target, slice);
    if (!output) return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input;
    if (FAILED(videoContext_->VideoProcessorBlt(processor_.Get(), output, 0, 1, &stream))) return false;
    frameReady_ = true;
    return true;
}

bool VideoEncoder::encodeConverted(int64_t ptsUs, const PacketSink& sink) {
    if (!codec_ || !frameReady_) return false;

    // Keyframe a cada 1 s de tempo real. O gop_size conta frames, e com captura VFR
    // (WGC só entrega frame quando a tela muda) isso faria os keyframes caírem em tempos irregulares.
    const bool requested = keyframeRequested_.exchange(false);
    const bool keyframe = requested || !hasKeyframe_ || ptsUs - lastKeyframePts_ >= 1000000;
    if (keyframe) {
        frame_->pict_type = AV_PICTURE_TYPE_I;
        lastKeyframePts_ = ptsUs;
        hasKeyframe_ = true;
    } else {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    frame_->pts = ptsUs;
    // O encoder guarda a própria referência da textura até terminar de codificá-la.
    const int sendResult = avcodec_send_frame(codec_, frame_);
    av_frame_unref(frame_);
    frameReady_ = false;
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
