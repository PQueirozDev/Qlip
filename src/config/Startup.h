#pragma once

// Inicialização com o Windows: entrada em HKCU\...\CurrentVersion\Run (só do usuário atual, sem admin).
namespace startup {

bool isEnabled();
bool setEnabled(bool enabled);

}  // namespace startup
