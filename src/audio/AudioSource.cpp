#include "audio/AudioSource.h"

#include <algorithm>
#include <future>

#include <wrl/client.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include "utils/Logger.h"
#include "utils/Text.h"

namespace {

using Microsoft::WRL::ComPtr;

// Converte um tempo do QPC em 100 ns (usado pelo WASAPI) para ticks do QPC, sem estourar 64 bits.
int64_t qpc100nsToTicks(uint64_t qpc100ns, int64_t frequency) {
    const auto seconds = static_cast<int64_t>(qpc100ns / 10000000ull);
    const auto remainder = static_cast<int64_t>(qpc100ns % 10000000ull);
    return seconds * frequency + remainder * frequency / 10000000;
}

int64_t currentTicks() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

struct ComScope {
    bool ok = false;
    ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope() { if (ok) CoUninitialize(); }
};

// Avisa quando o dispositivo padrão do Windows muda (ex.: trocou das caixas de som para o fone).
// Só é registrado quando a fonte acompanha o padrão (sem ID de dispositivo fixo).
class DefaultDeviceWatcher final : public IMMNotificationClient {
public:
    DefaultDeviceWatcher(EDataFlow flow, std::atomic<bool>& reconnect, HANDLE wake)
        : flow_(flow), reconnect_(reconnect), wake_(wake) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == flow_ && role == eConsole) {
            reconnect_ = true;
            SetEvent(wake_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> references_{1};
    EDataFlow flow_;
    std::atomic<bool>& reconnect_;
    HANDLE wake_;
};

struct Stream {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
};

// Abre o dispositivo (padrão ou pelo ID) em float32 estéreo 48 kHz, no modo de evento.
bool openStream(IMMDeviceEnumerator* enumerator, bool loopback, const std::wstring& deviceId, HANDLE wake,
                Stream& stream, std::wstring& reason) {
    ComPtr<IMMDevice> device;
    HRESULT hr = deviceId.empty() ? enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &device)
                                  : enumerator->GetDevice(deviceId.c_str(), &device);
    if (FAILED(hr)) {
        reason = loopback ? L"nenhum dispositivo de reprodução padrão."
                          : (deviceId.empty() ? L"nenhum microfone padrão (verifique a permissão de microfone)."
                                              : L"o microfone escolhido não está conectado.");
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &stream.client);
    if (FAILED(hr)) {
        reason = L"não foi possível ativar o cliente de áudio.";
        return false;
    }

    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = AudioRing::kChannels;
    format.Format.nSamplesPerSec = AudioSource::kSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = AudioRing::kChannels * 4;
    format.Format.nAvgBytesPerSec = AudioSource::kSampleRate * AudioRing::kChannels * 4;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200 * 10000 /* 200 ms em 100 ns */, 0, &format.Format, nullptr);
    if (FAILED(hr)) {
        reason = L"formato de áudio não suportado pelo dispositivo.";
        return false;
    }
    if (FAILED(stream.client->SetEventHandle(wake))) {
        reason = L"não foi possível configurar o evento de áudio.";
        return false;
    }
    if (FAILED(stream.client->GetService(IID_PPV_ARGS(&stream.capture)))) {
        reason = L"serviço de captura indisponível.";
        return false;
    }
    if (FAILED(stream.client->Start())) {
        reason = L"não foi possível iniciar a captura.";
        return false;
    }
    return true;
}

}  // namespace

AudioSource::~AudioSource() {
    stop();
}

bool AudioSource::start(int64_t startTicks, int64_t frequency, std::wstring* error) {
    if (running_) return true;

    // A inicialização roda na thread de captura (objetos COM do WASAPI não podem cruzar threads sem marshaling).
    std::promise<std::wstring> initResult;
    auto initFuture = initResult.get_future();

    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    running_ = true;
    reconnect_ = false;
    thread_ = std::thread([this, startTicks, frequency, promise = std::move(initResult)]() mutable {
        ComScope com;
        const bool loopback = kind_ == Kind::GameLoopback;
        const wchar_t* label = loopback ? L"áudio do jogo" : L"microfone";

        auto fail = [&](const std::wstring& reason) {
            QLOG_ERROR(std::wstring(L"Falha no ") + label + L": " + reason);
            promise.set_value(reason);
            running_ = false;
        };

        if (!com.ok) return fail(L"COM indisponível.");

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
            return fail(L"enumerador de áudio indisponível.");
        }

        // Acompanhando o padrão do Windows: avisa quando ele muda, para reabrir no novo dispositivo.
        DefaultDeviceWatcher* watcher = nullptr;
        if (deviceId_.empty()) {
            watcher = new DefaultDeviceWatcher(loopback ? eRender : eCapture, reconnect_, wakeEvent_);
            if (FAILED(enumerator->RegisterEndpointNotificationCallback(watcher))) {
                watcher->Release();
                watcher = nullptr;
            }
        }

        bool first = true;
        while (running_) {
            Stream stream;
            std::wstring reason;
            reconnect_ = false;
            if (!openStream(enumerator.Get(), loopback, deviceId_, wakeEvent_, stream, reason)) {
                if (first) {
                    fail(reason);
                    break;
                }
                // Dispositivo ainda ausente: tenta de novo em 1 s (o anel fica em silêncio nesse intervalo).
                WaitForSingleObject(wakeEvent_, 1000);
                continue;
            }

            if (first) {
                first = false;
                promise.set_value(L"");
                QLOG_INFO(std::wstring(L"Captura de ") + label + L" iniciada.");
            } else {
                QLOG_INFO(std::wstring(L"Captura de ") + label + L" reconectada.");
            }

            // Lê até parar, perder o dispositivo (erro do WASAPI) ou o padrão do Windows mudar.
            bool lost = false;
            while (running_ && !reconnect_ && !lost) {
                const DWORD wait = WaitForSingleObject(wakeEvent_, 200);
                if (!running_ || reconnect_) break;
                if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
                    lost = true;
                    break;
                }

                UINT32 packetFrames = 0;
                for (;;) {
                    HRESULT hr = stream.capture->GetNextPacketSize(&packetFrames);
                    if (FAILED(hr)) {
                        lost = true;  // ex.: AUDCLNT_E_DEVICE_INVALIDATED (fone desconectado)
                        break;
                    }
                    if (packetFrames == 0) break;

                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD bufferFlags = 0;
                    UINT64 devicePosition = 0;
                    UINT64 qpcPosition = 0;
                    hr = stream.capture->GetBuffer(&data, &frames, &bufferFlags, &devicePosition, &qpcPosition);
                    if (FAILED(hr)) {
                        lost = true;
                        break;
                    }

                    // Usa o QPC do WASAPI só se a flag de erro de timestamp não estiver marcada; senão, estima pelo instante atual.
                    const bool timestampValid = qpcPosition != 0 && (bufferFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0;
                    const int64_t ticks = timestampValid ? qpc100nsToTicks(qpcPosition, frequency)
                                                         : currentTicks() - frequency * static_cast<int64_t>(frames) / kSampleRate;
                    const int64_t startFrame = (ticks - startTicks) * kSampleRate / frequency;

                    if (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        static thread_local float silence[AudioRing::kChannels * 1024] = {};
                        for (UINT32 done = 0; done < frames; done += 1024) {
                            const int n = static_cast<int>(std::min<UINT32>(1024, frames - done));
                            ring_.write(startFrame + done, silence, n);
                        }
                    } else {
                        ring_.write(startFrame, reinterpret_cast<const float*>(data), static_cast<int>(frames));
                    }
                    stream.capture->ReleaseBuffer(frames);
                }
            }

            stream.client->Stop();
            if (running_) {
                QLOG_WARN(std::wstring(L"Dispositivo de ") + label +
                          (lost ? L" desconectado ou indisponível; tentando reconectar." : L" padrão mudou; reabrindo."));
            }
        }

        if (watcher) {
            enumerator->UnregisterEndpointNotificationCallback(watcher);
            watcher->Release();
        }
        if (!first) QLOG_INFO(std::wstring(L"Captura de ") + label + L" parada.");
    });

    const std::wstring reason = initFuture.get();
    if (!reason.empty()) {
        if (error) *error = reason;
        stop();
        return false;
    }
    return true;
}

void AudioSource::stop() {
    if (!thread_.joinable()) return;
    running_ = false;
    if (wakeEvent_) SetEvent(wakeEvent_);
    thread_.join();
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
}
