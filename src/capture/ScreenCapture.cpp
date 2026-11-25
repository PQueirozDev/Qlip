#include "capture/ScreenCapture.h"

#include <algorithm>

#include <d3d10.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include "utils/Logger.h"
#include "utils/Text.h"

namespace {

using Microsoft::WRL::ComPtr;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;

int64_t qpcNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

}  // namespace

ScreenCapture::~ScreenCapture() {
    stop();
}

bool ScreenCapture::start(HMONITOR monitor, HWND window, int maxFps, int slotCount, Callbacks callbacks, std::wstring* error) {
    callbacks_ = std::move(callbacks);

    // Exceções do WinRT não podem escapar: viram erro de início, com o estado parcial limpo.
    std::wstring reason;
    try {
        if (startInternal(monitor, window, maxFps, slotCount, &reason)) return true;
    } catch (const winrt::hresult_error& e) {
        reason = L"Falha na captura de tela: " + std::wstring(e.message());
    }

    if (error) *error = reason.empty() ? L"Falha ao iniciar a captura de tela." : reason;
    QLOG_ERROR(reason.empty() ? L"Falha ao iniciar a captura de tela." : reason);
    stop();
    return false;
}

bool ScreenCapture::startInternal(HMONITOR monitor, HWND window, int maxFps, int slotCount, std::wstring* error) {
    auto fail = [error](const wchar_t* message) {
        if (error) *error = message;
        return false;
    };

    if (!WGC::GraphicsCaptureSession::IsSupported()) {
        return fail(L"Captura de tela (Windows Graphics Capture) não é suportada neste Windows.");
    }

    // Dispositivo D3D11 com BGRA (interoperabilidade com WinRT) e vídeo (conversão BGRA -> NV12 na GPU).
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) return fail(L"GPU indisponível para captura (D3D11CreateDevice falhou).");
    // O contexto é usado pela captura, pelo encoder e pelo NVENC (via FFmpeg): o próprio D3D também serializa.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgiDevice;
    device_.As(&dxgiDevice);
    winrt::com_ptr<::IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
    if (FAILED(hr)) return fail(L"Falha ao criar o dispositivo WinRT para captura.");
    winrtDevice_ = inspectable.as<WGD::Direct3D11::IDirect3DDevice>();

    // Item de captura: monitor ou janela.
    auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (window) {
        hr = interop->CreateForWindow(window, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                                      winrt::put_abi(item_));
        if (FAILED(hr) || !item_) return fail(L"Não foi possível capturar a janela selecionada.");
    } else {
        hr = interop->CreateForMonitor(monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                                       winrt::put_abi(item_));
        if (FAILED(hr) || !item_) return fail(L"Não foi possível capturar o monitor selecionado.");
    }

    const auto size = item_.Size();
    width_ = static_cast<uint32_t>(size.Width);
    height_ = static_cast<uint32_t>(size.Height);

    // Anel de texturas na GPU: o frame do WGC é copiado para um slot e o pool do WGC fica livre logo.
    slots_.clear();
    slotWidth_.clear();
    slotHeight_.clear();
    {
        std::lock_guard lock(slotMutex_);
        freeSlots_.clear();
    }
    for (int i = 0; i < slotCount; ++i) {
        slots_.emplace_back();
        slotWidth_.push_back(0);
        slotHeight_.push_back(0);
        if (!ensureSlotSize(i, width_, height_)) return fail(L"Memória de GPU insuficiente para os buffers de captura.");
        std::lock_guard lock(slotMutex_);
        freeSlots_.push_back(i);
    }

    // Limita a taxa aceita: WGC pode entregar mais frames que o FPS configurado.
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    minIntervalTicks_ = freq.QuadPart / std::max(1, maxFps);
    lastAcceptedTicks_ = 0;

    framePool_ = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice_, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);

    // running_ é ligado antes dos eventos: um Closed que chegue logo depois não pode ser sobrescrito.
    {
        std::lock_guard lock(frameMutex_);
        running_ = true;
    }
    frameArrivedToken_ = framePool_.FrameArrived([this](auto&&, auto&&) { onFrameArrived(); });
    const bool isWindowTarget = window != nullptr;
    closedToken_ = item_.Closed([this, isWindowTarget](auto&&, auto&&) {
        std::lock_guard lock(frameMutex_);
        running_ = false;
        if (callbacks_.onFatal) {
            callbacks_.onFatal(isWindowTarget ? L"A janela de captura foi fechada." : L"O monitor foi desconectado ou removido.");
        }
    });

    session_ = framePool_.CreateCaptureSession(item_);
    try {
        // Remove a borda amarela do Windows 11 quando disponível.
        session_.IsBorderRequired(false);
    } catch (const winrt::hresult_error&) {
        // Propriedade ausente em versões antigas do Windows: a borda continua, sem impacto na gravação.
    }
    session_.IsCursorCaptureEnabled(true);
    session_.StartCapture();

    QLOG_INFO(std::wstring(window ? L"Captura de janela iniciada: " : L"Captura de monitor iniciada: ") +
              std::to_wstring(width_.load()) + L"x" + std::to_wstring(height_.load()));
    return true;
}

void ScreenCapture::stop() {
    if (!framePool_) return;

    // Revoga os eventos antes de fechar: nenhuma callback nova começa depois daqui.
    framePool_.FrameArrived(frameArrivedToken_);
    item_.Closed(closedToken_);

    if (session_) session_.Close();
    framePool_.Close();

    // Espera a callback em andamento (se houver) terminar, sob o mesmo lock que ela usa.
    {
        std::lock_guard lock(frameMutex_);
        running_ = false;
    }

    session_ = nullptr;
    framePool_ = nullptr;
    item_ = nullptr;
    callbacks_ = {};
    QLOG_INFO(L"Captura parada.");
}

bool ScreenCapture::ensureSlotSize(int slot, uint32_t width, uint32_t height) {
    if (slots_[slot] && slotWidth_[slot] == width && slotHeight_[slot] == height) return true;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // Entrada do processador de vídeo (conversão de cor e escala) do encoder.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture))) return false;
    slots_[slot] = texture;
    slotWidth_[slot] = width;
    slotHeight_[slot] = height;
    return true;
}

void ScreenCapture::onFrameArrived() {
    // Callback da threadpool do WinRT: exceção não tratada aqui encerra o processo.
    try {
        std::lock_guard lock(frameMutex_);
        if (!running_) return;

        auto frame = framePool_.TryGetNextFrame();
        if (!frame) return;

        const int64_t now = qpcNow();
        if (lastAcceptedTicks_ != 0 && now - lastAcceptedTicks_ < minIntervalTicks_) {
            return;  // Acima do FPS configurado: descarta sem custo de cópia.
        }

        // Resize (monitor mudou de resolução ou janela foi redimensionada): recria o pool.
        // O encoder escala para o tamanho de saída, então a gravação continua sem reiniciar.
        const auto contentSize = frame.ContentSize();
        const auto newWidth = static_cast<uint32_t>(contentSize.Width);
        const auto newHeight = static_cast<uint32_t>(contentSize.Height);
        if (newWidth != width_ || newHeight != height_) {
            width_ = newWidth;
            height_ = newHeight;
            framePool_.Recreate(winrtDevice_, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
            QLOG_INFO(L"Resolução da captura mudou para " + std::to_wstring(width_.load()) + L"x" + std::to_wstring(height_.load()));
        }

        // Pega a textura antes de reservar o slot: assim uma falha aqui não consome slot.
        auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(&texture)))) return;

        const int slot = acquireSlot();
        if (slot < 0) {
            // Consumidor atrasado: nenhum slot livre. O receptor conta como frame perdido.
            if (callbacks_.onFrame) callbacks_.onFrame(Frame{-1, width_, height_, now});
            return;
        }

        {
            std::lock_guard d3d(d3dMutex_);
            // Slot livre: é seguro trocar a textura dele se o tamanho mudou.
            if (!ensureSlotSize(slot, width_, height_)) {
                releaseSlot(slot);
                return;
            }
            // Logo após um resize, a superfície do frame pode ter o tamanho antigo: copia só a área em comum.
            D3D11_TEXTURE2D_DESC source{};
            texture->GetDesc(&source);
            const D3D11_BOX box{0, 0, 0, std::min<UINT>(source.Width, width_), std::min<UINT>(source.Height, height_), 1};
            context_->CopySubresourceRegion(slots_[slot].Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
        }

        lastAcceptedTicks_ = now;
        if (callbacks_.onFrame) callbacks_.onFrame(Frame{slot, width_, height_, now});
    } catch (const winrt::hresult_error& e) {
        QLOG_ERROR(L"Erro ao processar frame da captura: " + std::wstring(e.message()));
        std::lock_guard lock(frameMutex_);
        running_ = false;
        if (callbacks_.onFatal) callbacks_.onFatal(L"Erro na captura de tela: " + std::wstring(e.message()));
    }
}

int ScreenCapture::acquireSlot() {
    std::lock_guard lock(slotMutex_);
    if (freeSlots_.empty()) return -1;
    const int slot = freeSlots_.front();
    freeSlots_.pop_front();
    return slot;
}

void ScreenCapture::releaseSlot(int slot) {
    if (slot < 0) return;
    std::lock_guard lock(slotMutex_);
    freeSlots_.push_back(slot);
}

bool ScreenCapture::useSlot(int slot, const std::function<bool(ID3D11Texture2D*)>& fn) {
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return false;
    std::lock_guard d3d(d3dMutex_);
    // A ordem do contexto garante que a cópia do frame termina na GPU antes do que fn enfileirar.
    return fn(slots_[slot].Get());
}
