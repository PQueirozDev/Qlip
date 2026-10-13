#include <windows.h>
#include <winrt/base.h>

#include "app/Application.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, LPWSTR /*cmdLine*/, int /*show*/) {
    // Ativa DPI por monitor para a janela não ficar borrada e para a captura usar resolução física.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Necessário para Windows Graphics Capture (WinRT).
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    Application app(instance);
    return app.run();
}
