#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Captura via Windows Graphics Capture: um monitor (área de trabalho) ou uma janela (jogo).
// Os frames são copiados na GPU para um anel de texturas (BGRA); o encoder converte e codifica direto dessas
// texturas. A imagem nunca passa pela RAM: não há leitura da GPU para a CPU em nenhum ponto.
class ScreenCapture {
public:
    struct Frame {
        int slot = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t qpcTicks = 0;
    };

    struct Callbacks {
        // Chamado na thread de captura. O receptor DEVE chamar releaseSlot() quando não precisar mais do slot.
        std::function<void(const Frame&)> onFrame;
        // Chamado quando o alvo some (monitor desconectado ou janela fechada).
        // Roda dentro da callback de captura, com o lock interno segurado: NÃO chamar stop() daqui.
        std::function<void(const std::wstring& reason)> onFatal;
    };

    ScreenCapture() = default;
    ~ScreenCapture();

    // Captura um monitor (window == nullptr) ou uma janela (window != nullptr).
    bool start(HMONITOR monitor, HWND window, int maxFps, int slotCount, Callbacks callbacks, std::wstring* error);
    void stop();

    // Chama fn com a textura do slot, com o contexto D3D travado (fn pode enfileirar trabalho de GPU nele).
    // O slot continua reservado (ex.: para repetir o frame no CFR) até releaseSlot().
    bool useSlot(int slot, const std::function<bool(ID3D11Texture2D* texture)>& fn);
    void releaseSlot(int slot);

    // Dispositivo D3D11 da captura: o encoder usa o mesmo, para ler as texturas sem cópia entre GPUs/dispositivos.
    ID3D11Device* device() const { return device_.Get(); }

    // Tamanho atual do alvo (muda se o monitor ou a janela mudar de tamanho).
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    bool startInternal(HMONITOR monitor, HWND window, int maxFps, int slotCount, std::wstring* error);
    void onFrameArrived();
    int acquireSlot();
    bool ensureSlotSize(int slot, uint32_t width, uint32_t height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> slots_;
    std::vector<uint32_t> slotWidth_;
    std::vector<uint32_t> slotHeight_;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::event_token frameArrivedToken_{};
    winrt::event_token closedToken_{};

    Callbacks callbacks_;
    // Atômicos: a callback de frame atualiza no resize enquanto a thread principal lê o tamanho.
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    int64_t minIntervalTicks_ = 0;
    int64_t lastAcceptedTicks_ = 0;
    bool running_ = false;

    // Serializa o uso do contexto D3D (cópia na captura, conversão de cor no encoder).
    std::mutex d3dMutex_;
    // Protege a lista de slots livres.
    std::mutex slotMutex_;
    std::deque<int> freeSlots_;
    // Impede que a callback de frame rode durante stop().
    std::mutex frameMutex_;
};
