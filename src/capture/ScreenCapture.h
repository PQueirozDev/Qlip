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
// Os frames são copiados na GPU para um anel de texturas de staging; o consumidor lê um slot por vez.
// Não há cópia para RAM dentro da captura: a leitura só acontece quando o encoder pede.
class ScreenCapture {
public:
    struct Frame {
        int slot = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t qpcTicks = 0;
    };

    struct Callbacks {
        // Chamado na thread de captura. O receptor DEVE chamar consumeSlot(), viewSlot()+releaseSlot() ou releaseSlot().
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

    // Lê o slot, chama fn(data, rowPitch) e libera o slot.
    void consumeSlot(int slot, const std::function<void(const uint8_t* data, int rowPitch)>& fn);
    // Lê o slot sem liberá-lo (o frame continua reservado, ex.: para repetir no CFR).
    bool viewSlot(int slot, const std::function<void(const uint8_t* data, int rowPitch)>& fn);
    // Libera um slot sem ler.
    void releaseSlot(int slot);

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
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> staging_;
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

    // Serializa o uso do contexto D3D (CopyResource na captura, Map/Unmap no consumidor).
    std::mutex d3dMutex_;
    // Protege a lista de slots livres.
    std::mutex slotMutex_;
    std::deque<int> freeSlots_;
    // Impede que a callback de frame rode durante stop().
    std::mutex frameMutex_;
};
