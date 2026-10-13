#include "recording/VideoMuxer.h"

#include <vector>

#include "utils/Text.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace {

std::string errorText(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// Tamanho do prefixo de comprimento dos NALs (formato avcC do H.264), ou 0 se o fluxo não usa esse formato.
int avccLengthSize(const AVCodecParameters* params) {
    if (params->codec_id != AV_CODEC_ID_H264 || params->extradata_size < 7 || params->extradata[0] != 1) return 0;
    return (params->extradata[4] & 0x03) + 1;
}

// Um pacote cortado (gravação interrompida no meio da escrita) declara NALs maiores do que os dados que tem.
bool nalUnitsFit(const AVPacket* packet, int lengthSize) {
    int offset = 0;
    while (lengthSize <= packet->size - offset) {  // sem soma que possa estourar
        uint32_t length = 0;
        for (int i = 0; i < lengthSize; ++i) length = (length << 8) | packet->data[offset + i];
        offset += lengthSize;
        // NAL de tamanho zero não existe em H.264: indica área zerada (arquivo cortado), não dado válido.
        if (length == 0 || length > static_cast<uint32_t>(packet->size - offset)) return false;
        offset += static_cast<int>(length);
    }
    return offset == packet->size;
}

}  // namespace

VideoMuxer::~VideoMuxer() {
    std::string ignored;
    close(&ignored);
}

bool VideoMuxer::open(const std::filesystem::path& file, const AVCodecContext* videoCodec, const AVCodecContext* audioCodec, std::string* error) {
    close(nullptr);
    auto fail = [this, error](std::string message) {
        if (error) *error = std::move(message);
        close(nullptr);
        return false;
    };

    // MP4 fragmentado: preserva a base de tempo do encoder (o Matroska arredonda para 1 ms, o que tirava o CFR
    // exato) e continua tolerante a queda, já que cada fragmento fecha num keyframe.
    const std::string path = toUtf8(file.wstring());
    int result = avformat_alloc_output_context2(&format_, nullptr, "mp4", path.c_str());
    if (result < 0 || !format_) return fail("Falha ao criar o arquivo de gravação: " + errorText(result));

    const AVCodecContext* codecs[2] = {videoCodec, audioCodec};
    for (int i = 0; i < 2; ++i) {
        if (!codecs[i]) continue;
        timeBaseNum_[i] = codecs[i]->time_base.num;
        timeBaseDen_[i] = codecs[i]->time_base.den;

        streams_[i] = avformat_new_stream(format_, nullptr);
        if (!streams_[i]) return fail("Falha ao criar um fluxo de gravação.");
        result = avcodec_parameters_from_context(streams_[i]->codecpar, codecs[i]);
        if (result < 0) return fail("Falha ao copiar os parâmetros do fluxo: " + errorText(result));
        // Sem atraso de reordenação: o muxer não deve recalcular o dts a partir de um atraso do encoder.
        streams_[i]->codecpar->video_delay = 0;
        // Mesma base de tempo do encoder: nenhum arredondamento dos timestamps no arquivo intermediário.
        streams_[i]->time_base = codecs[i]->time_base;
    }

    result = avio_open(&format_->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (result < 0) return fail("Não foi possível criar o arquivo (" + errorText(result) + "). Verifique a pasta e o espaço em disco.");

    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
    result = avformat_write_header(format_, &options);
    av_dict_free(&options);
    if (result < 0) return fail("Falha ao escrever o cabeçalho do arquivo: " + errorText(result));
    headerWritten_ = true;

    return true;
}

bool VideoMuxer::write(AVPacket* packet, std::string* error) {
    const int index = packet->stream_index;
    if (!format_ || index < 0 || index > 1 || !streams_[index]) {
        av_packet_free(&packet);
        if (error) *error = "Fluxo de gravação inválido.";
        return false;
    }

    // Converte do time base do encoder para o time base escolhido pelo contêiner.
    av_packet_rescale_ts(packet, AVRational{timeBaseNum_[index], timeBaseDen_[index]}, streams_[index]->time_base);
    packet->stream_index = streams_[index]->index;

    // av_interleaved_write_frame esvazia o pacote, mas não libera a struct: ela é liberada aqui.
    const int result = av_interleaved_write_frame(format_, packet);
    av_packet_free(&packet);
    if (result < 0) {
        if (error) *error = "Falha ao gravar no disco (" + errorText(result) + ").";
        return false;
    }
    return true;
}

bool VideoMuxer::close(std::string* error) {
    if (!format_) return true;

    bool ok = true;
    if (headerWritten_) {
        const int result = av_write_trailer(format_);
        if (result < 0) {
            ok = false;
            if (error) *error = "Falha ao finalizar o arquivo (" + errorText(result) + ").";
        }
    }
    // O fechamento faz o flush final: falha aqui (ex.: disco cheio) significa arquivo incompleto.
    if (format_->pb) {
        const int closeResult = avio_closep(&format_->pb);
        if (closeResult < 0 && ok) {
            ok = false;
            if (error) *error = "Falha ao salvar o arquivo no disco (" + errorText(closeResult) + ").";
        }
    }
    avformat_free_context(format_);
    format_ = nullptr;
    streams_[0] = streams_[1] = nullptr;
    headerWritten_ = false;
    return ok;
}

bool VideoMuxer::remux(const std::filesystem::path& src, const std::filesystem::path& dst, std::string* error,
                       int* skippedPackets) {
    // skipped: pacotes ruins seguidos de vídeo válido (corrupção no meio). pendingSkips: os do fim do arquivo,
    // esperados numa gravação interrompida, que podem ser descartados sem preservar o original.
    int skipped = 0;
    int pendingSkips = 0;
    if (skippedPackets) *skippedPackets = 0;
    auto fail = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };

    const std::string srcPath = toUtf8(src.wstring());
    const std::filesystem::path tempPath = dst.wstring() + L".tmp";
    const std::string tmpPath = toUtf8(tempPath.wstring());

    AVFormatContext* input = nullptr;
    int result = avformat_open_input(&input, srcPath.c_str(), nullptr, nullptr);
    if (result < 0) return fail("Arquivo de gravação ilegível (" + errorText(result) + ").");

    AVFormatContext* output = nullptr;
    auto cleanup = [&] {
        if (input) avformat_close_input(&input);
        if (output) {
            if (output->pb) avio_closep(&output->pb);
            avformat_free_context(output);
        }
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    };

    result = avformat_find_stream_info(input, nullptr);
    if (result < 0) {
        cleanup();
        return fail("Não foi possível ler os fluxos do arquivo (" + errorText(result) + ").");
    }

    result = avformat_alloc_output_context2(&output, nullptr, "mp4", tmpPath.c_str());
    if (result < 0 || !output) {
        cleanup();
        return fail("Falha ao criar o MP4 (" + errorText(result) + ").");
    }

    // Mapeia cada fluxo de entrada para um de saída (vídeo e áudio; o resto é ignorado).
    std::vector<int> streamMap(input->nb_streams, -1);
    std::vector<int> lengthSizes(input->nb_streams, 0);
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        AVStream* inStream = input->streams[i];
        lengthSizes[i] = avccLengthSize(inStream->codecpar);
        if (inStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && inStream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        AVStream* outStream = avformat_new_stream(output, nullptr);
        if (!outStream) {
            cleanup();
            return fail("Falha ao criar fluxo de saída.");
        }
        result = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if (result < 0) {
            cleanup();
            return fail("Falha ao copiar parâmetros de fluxo (" + errorText(result) + ").");
        }
        outStream->codecpar->codec_tag = 0;
        outStream->codecpar->video_delay = 0;
        outStream->time_base = inStream->time_base;  // mantém a precisão dos timestamps (CFR exato)
        streamMap[i] = outStream->index;
    }

    result = avio_open(&output->pb, tmpPath.c_str(), AVIO_FLAG_WRITE);
    if (result < 0) {
        cleanup();
        return fail("Não foi possível criar o MP4 (" + errorText(result) + "). Verifique o espaço em disco.");
    }

    // Mantém o moov no início: o arquivo toca mesmo se o app fechar antes do trailer.
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0);
    result = avformat_write_header(output, &options);
    av_dict_free(&options);
    if (result < 0) {
        cleanup();
        return fail("Falha ao escrever o cabeçalho do MP4 (" + errorText(result) + ").");
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        cleanup();
        return fail("Memória insuficiente para recuperar a gravação.");
    }

    // Fim normal do arquivo é AVERROR_EOF; qualquer outro código é erro de leitura (arquivo corrompido).
    while ((result = av_read_frame(input, packet)) >= 0) {
        const int outIndex = packet->stream_index < static_cast<int>(streamMap.size()) ? streamMap[packet->stream_index] : -1;
        if (outIndex < 0) {
            av_packet_unref(packet);
            continue;
        }
        // Pacote cortado (gravação interrompida no meio da escrita): não é copiado. A leitura continua,
        // e o chamador preserva o original quando algum pacote foi pulado.
        const int lengthSize = lengthSizes[packet->stream_index];
        if (lengthSize > 0) {
            if (!nalUnitsFit(packet, lengthSize)) {
                ++pendingSkips;
                av_packet_unref(packet);
                continue;
            }
            // Vídeo válido depois de pacotes ruins: a corrupção estava no meio, não só no fim do arquivo.
            skipped += pendingSkips;
            pendingSkips = 0;
        }
        AVStream* inStream = input->streams[packet->stream_index];
        AVStream* outStream = output->streams[outIndex];
        av_packet_rescale_ts(packet, inStream->time_base, outStream->time_base);
        packet->stream_index = outIndex;
        packet->pos = -1;
        const int writeResult = av_interleaved_write_frame(output, packet);
        if (writeResult < 0) {
            av_packet_free(&packet);
            cleanup();
            return fail("Falha ao gravar o MP4 (" + errorText(writeResult) + "). Verifique o espaço em disco.");
        }
    }
    av_packet_free(&packet);
    if (result != AVERROR_EOF) {
        cleanup();
        return fail("Arquivo de gravação corrompido ou incompleto (" + errorText(result) + ").");
    }

    result = av_write_trailer(output);
    if (result < 0) {
        cleanup();
        return fail("Falha ao finalizar o MP4 (" + errorText(result) + ").");
    }

    // Só publica o arquivo se o fechamento (flush final em disco) deu certo.
    const int closeResult = avio_closep(&output->pb);
    avformat_free_context(output);
    output = nullptr;
    avformat_close_input(&input);
    input = nullptr;
    if (closeResult < 0) {
        cleanup();
        return fail("Falha ao salvar o MP4 no disco (" + errorText(closeResult) + ").");
    }

    std::error_code ec;
    if (skippedPackets) *skippedPackets = skipped;
    std::filesystem::rename(tempPath, dst, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return fail("Não foi possível salvar o arquivo final: " + ec.message());
    }
    return true;
}
