#pragma once

#include <string>
#include <vector>

#include <windows.h>

// Monitores conectados, identificados pelo nome do dispositivo (ex.: \\.\DISPLAY2), que é estável entre execuções
// enquanto o monitor fica na mesma saída de vídeo.
struct MonitorEntry {
    std::wstring deviceName;
    std::wstring label;  // ex.: "Monitor 2 · 2560×1440"
    bool primary = false;
};

std::vector<MonitorEntry> listMonitors();

// Monitor com esse nome de dispositivo, ou nullptr se não estiver conectado.
HMONITOR findMonitor(const std::wstring& deviceName);
