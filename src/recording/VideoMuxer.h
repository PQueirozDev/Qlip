#pragma once

#include <filesystem>
#include <string>

struct AVCodecContext;
struct AVFormatContext;
struct AVPacket;
struct AVStream;

// Escreve pacotes em um MP4 fragmentado temporário (vídeo H.264 e, opcionalmente, áudio AAC) e faz o remux
// para MP4 normal ao final. Fragmentado é tolerante a queda: se o app fechar no meio, o arquivo continua recuperável.
// Pacotes: stream_index 0 = vídeo, 1 = áudio.
class VideoMuxer {
public:
    static constexpr int kVideoStream = 0;
    static constexpr int kAudioStream = 1;

    VideoMuxer() = default;
    ~VideoMuxer();

    // audioCodec pode ser nullptr (gravação sem áudio).
    bool open(const std::filesystem::path& file, const AVCodecContext* videoCodec, const AVCodecContext* audioCodec, std::string* error);
    // Assume a posse do pacote (é liberado ou consumido pela escrita).
    bool write(AVPacket* packet, std::string* error);
    bool close(std::string* error);

    // Copia os fluxos de src para dst (MP4) sem recodificar. Escreve em arquivo temporário e renomeia.
    // skippedPackets (opcional): pacotes inválidos pulados no meio do arquivo (havia vídeo válido depois deles).
    // Se > 0, o chamador deve preservar o arquivo de origem. Corte só no fim (queda) não conta.
    static bool remux(const std::filesystem::path& src, const std::filesystem::path& dst, std::string* error,
                      int* skippedPackets = nullptr);

private:
    AVFormatContext* format_ = nullptr;
    AVStream* streams_[2] = {nullptr, nullptr};
    int timeBaseNum_[2] = {1, 1};
    int timeBaseDen_[2] = {1, 1};
    // O trailer só é escrito se o cabeçalho foi escrito com sucesso.
    bool headerWritten_ = false;
};
