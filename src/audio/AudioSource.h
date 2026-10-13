#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include <windows.h>

#include "audio/AudioRing.h"

// Captura WASAPI de uma fonte de áudio para um AudioRing:
//  - Loopback: o que sai pelo dispositivo de reprodução padrão (áudio do jogo/sistema).
//  - Microfone: o dispositivo de captura padrão ou um escolhido pelo ID.
// A thread própria inicializa o cliente e lê os pacotes; o formato é pedido já em float32 estéreo 48 kHz,
// e o WASAPI converte se o dispositivo for diferente (AUTOCONVERTPCM).
// Se o dispositivo for desconectado ou o padrão do Windows mudar, o fluxo é reaberto sozinho; o intervalo
// sem dispositivo vira silêncio no anel (o tempo continua vindo do QPC, então a sincronia é mantida).
class AudioSource {
public:
    enum class Kind { GameLoopback, Microphone };

    static constexpr int kSampleRate = 48000;

    // deviceId vazio = dispositivo padrão do Windows (e acompanha quando o padrão muda).
    AudioSource(Kind kind, AudioRing& ring, std::wstring deviceId = {})
        : kind_(kind), ring_(ring), deviceId_(std::move(deviceId)) {}
    ~AudioSource();

    // Inicia a captura. startTicks/frequency definem o zero do tempo (mesmo QPC do vídeo).
    // Falha só se o dispositivo não abrir na primeira tentativa; depois disso, quedas são reconectadas.
    bool start(int64_t startTicks, int64_t frequency, std::wstring* error);
    void stop();

private:
    Kind kind_;
    AudioRing& ring_;
    std::wstring deviceId_;
    std::thread thread_;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> reconnect_{false};  // pedido de reabrir o fluxo (padrão do Windows mudou)
};
