#include "capture/Monitors.h"

namespace {

struct EnumState {
    std::vector<MonitorEntry> monitors;
    std::wstring wanted;
    HMONITOR found = nullptr;
};

BOOL CALLBACK collect(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* state = reinterpret_cast<EnumState*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;

    if (!state->wanted.empty()) {
        if (state->wanted == info.szDevice) {
            state->found = monitor;
            return FALSE;  // achou: para a enumeração
        }
        return TRUE;
    }

    MonitorEntry entry;
    entry.deviceName = info.szDevice;
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    entry.label = L"Monitor " + std::to_wstring(state->monitors.size() + 1) + L" · " + std::to_wstring(width) + L"×" +
                  std::to_wstring(height) + (entry.primary ? L" (principal)" : L"");
    state->monitors.push_back(entry);
    return TRUE;
}

}  // namespace

std::vector<MonitorEntry> listMonitors() {
    EnumState state;
    EnumDisplayMonitors(nullptr, nullptr, &collect, reinterpret_cast<LPARAM>(&state));
    return state.monitors;
}

HMONITOR findMonitor(const std::wstring& deviceName) {
    if (deviceName.empty()) return nullptr;
    EnumState state;
    state.wanted = deviceName;
    EnumDisplayMonitors(nullptr, nullptr, &collect, reinterpret_cast<LPARAM>(&state));
    return state.found;
}
