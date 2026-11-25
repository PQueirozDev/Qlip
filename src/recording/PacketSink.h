#pragma once

struct AVPacket;

// Destino dos pacotes codificados de um CapturePipeline (arquivo de gravação, buffer de replay).
// Todos os métodos são chamados na thread de escrita do pipeline, nunca ao mesmo tempo.
class PacketSink {
public:
    virtual ~PacketSink() = default;

    // Vídeo: stream 0, tempo em microssegundos. Áudio: stream 1, tempo em amostras de 1/48000 s.
    // Não assume a posse: quem precisar guardar o pacote faz av_packet_clone.
    virtual void onPacket(const AVPacket* packet) = 0;
    // true = o destino terminou sozinho (ex.: a gravação chegou ao instante do stop). O pipeline o remove e chama onEnd.
    virtual bool finished() const { return false; }
    // Último aviso, uma vez por anexação: o destino terminou ou o pipeline parou (stop ou falha).
    // Não é chamado quando o próprio dono remove o destino com CapturePipeline::removeSink.
    virtual void onEnd() = 0;
};
