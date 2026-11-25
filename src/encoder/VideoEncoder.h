#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// Encoder H.264 por hardware (NVENC, via h264_nvenc do libavcodec), todo na GPU:
// a textura BGRA da captura é convertida para NV12 (com escala) pelo processador de vídeo do D3D11, numa
// textura que o NVENC lê direto. Nenhum pixel passa pela CPU.
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

    // device: o dispositivo D3D11 da captura (as texturas de origem precisam ser dele).
    bool open(const Config& config, ID3D11Device* device, std::string* error);
    void close();

    // Converte a textura BGRA para NV12 no tamanho de saída. Enfileira trabalho no contexto D3D:
    // chamar com o contexto travado (dentro de ScreenCapture::useSlot).
    bool convert(ID3D11Texture2D* bgra, int srcWidth, int srcHeight);
    // Codifica o frame da última convert(). ptsUs: tempo do frame em microssegundos desde o início.
    bool encodeConverted(int64_t ptsUs, const PacketSink& sink);
    // Envia o restante dos pacotes que o encoder ainda guarda.
    bool flush(const PacketSink& sink);

    // O próximo frame sai como IDR (ex.: uma gravação começando no meio de um pipeline já ativo).
    void requestKeyframe() { keyframeRequested_ = true; }

    // Parâmetros usados pelo muxer (SPS/PPS e time base).
    const AVCodecContext* codecContext() const { return codec_; }

private:
    bool drain(const PacketSink& sink);
    bool ensureProcessor(int srcWidth, int srcHeight);
    ID3D11VideoProcessorOutputView* outputView(ID3D11Texture2D* texture, UINT arraySlice);

    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    AVBufferRef* hwFrames_ = nullptr;
    AVFrame* frame_ = nullptr;   // frame convertido, à espera de encodeConverted()
    bool frameReady_ = false;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    int processorSrcWidth_ = 0;
    int processorSrcHeight_ = 0;
    // Views por textura (as texturas ficam referenciadas aqui: o ponteiro nunca é reaproveitado por outra).
    struct InputView {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> view;
    };
    std::vector<InputView> inputViews_;
    std::map<UINT, Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>> outputViews_;

    int width_ = 0;
    int height_ = 0;
    int64_t lastKeyframePts_ = 0;
    bool hasKeyframe_ = false;
    std::atomic<bool> keyframeRequested_{false};
};
