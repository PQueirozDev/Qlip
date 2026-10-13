#pragma once

#include <string>
#include <vector>

// Dispositivos de captura de áudio (microfones) ativos, com o ID do WASAPI e o nome amigável.
struct AudioDeviceEntry {
    std::wstring id;
    std::wstring name;
};

// Precisa de COM inicializado na thread que chama (a thread da UI já tem).
std::vector<AudioDeviceEntry> listCaptureDevices();
