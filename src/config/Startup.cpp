#include "config/Startup.h"

#include <string>

#include <windows.h>

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"Qlip";
constexpr wchar_t kLegacyValueName[] = L"QClip";  // nome antigo do app: removido para não iniciar duas vezes

std::wstring executableCommand() {
    // Buffer grande: caminho truncado gravaria um comando que não existe.
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    // --background: ao ligar o PC, o Qlip abre em silêncio (sem mostrar o painel).
    return L"\"" + path + L"\" " + std::wstring(startup::kBackgroundArg);
}

}  // namespace

namespace startup {

bool isEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    const LSTATUS status = RegQueryValueExW(key, kValueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool setEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    RegDeleteValueW(key, kLegacyValueName);  // entrada do nome antigo, se existir
    bool ok = true;
    if (enabled) {
        const std::wstring command = executableCommand();
        if (command.empty()) {
            RegCloseKey(key);
            return false;
        }
        ok = RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        const LSTATUS status = RegDeleteValueW(key, kValueName);
        ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return ok;
}

}  // namespace startup
