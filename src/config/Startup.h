#pragma once

// Inicialização com o Windows: entrada em HKCU\...\CurrentVersion\Run (só do usuário atual, sem admin).
namespace startup {

// Argumento da entrada de inicialização: abre sem mostrar o painel.
inline constexpr wchar_t kBackgroundArg[] = L"--background";

bool isEnabled();
bool setEnabled(bool enabled);

}  // namespace startup
