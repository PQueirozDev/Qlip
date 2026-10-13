#include "config/SettingsManager.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <type_traits>

#include <objbase.h>
#include <shlobj.h>
#include <windows.h>
#include <nlohmann/json.hpp>

#include "utils/Text.h"

namespace {

template <typename T>
T readOr(const nlohmann::json& j, const char* key, T fallback) {
    if (!j.contains(key)) return fallback;
    const auto& value = j.at(key);
    // Para inteiros (não bool), rejeita valores fracionários (ex.: 30.5) em vez de truncar.
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (!value.is_number_integer()) return fallback;
    }
    try {
        return value.template get<T>();
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

// Mantém o valor só se estiver entre as opções oferecidas na interface; senão usa o padrão.
int oneOf(int value, std::initializer_list<int> allowed, int fallback) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end() ? value : fallback;
}

int clampInt(int value, int low, int high) {
    return std::clamp(value, low, high);
}

std::filesystem::path knownVideosFolder() {
    wchar_t* videos = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) {
        base = videos;
        CoTaskMemFree(videos);
    }
    return base;
}

}  // namespace

int qualityBitrateKbps(int qualityIndex) {
    static constexpr int kBitrates[] = {6000, 12000, 25000, 40000};
    return kBitrates[clampInt(qualityIndex, 0, 3)];
}

std::filesystem::path SettingsManager::appDataDir() {
    wchar_t* roaming = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        base = roaming;
        CoTaskMemFree(roaming);
    }
    return base / L"Qlip";
}

void SettingsManager::migrateLegacyAppData() {
    // O app se chamava QClip: na primeira execução como Qlip, a pasta antiga (configurações e logs) é movida.
    const std::filesystem::path current = appDataDir();
    const std::filesystem::path legacy = current.parent_path() / L"QClip";
    std::error_code ec;
    if (!std::filesystem::exists(current, ec) && std::filesystem::exists(legacy, ec)) {
        std::filesystem::rename(legacy, current, ec);
    }
}

std::filesystem::path SettingsManager::settingsFile() {
    return appDataDir() / L"settings.json";
}

std::filesystem::path SettingsManager::recordingsFolder() const {
    if (!settings_.recordingsFolder.empty()) return settings_.recordingsFolder;
    return knownVideosFolder() / L"Qlip";
}

std::filesystem::path SettingsManager::clipsFolder() const {
    if (!settings_.clipsFolder.empty()) return settings_.clipsFolder;
    return knownVideosFolder() / L"Qlip" / L"Clipes";
}

bool SettingsManager::load() {
    settings_ = Settings{};

    std::ifstream in(settingsFile());
    if (!in) return false;

    try {
        const auto j = nlohmann::json::parse(in, nullptr, true, true);
        const Settings defaults;

        settings_.recordFps = oneOf(readOr(j, "recordFps", defaults.recordFps), {30, 60, 120}, defaults.recordFps);
        settings_.recordQuality = clampInt(readOr(j, "recordQuality", defaults.recordQuality), 0, 3);
        settings_.resolutionHeight = oneOf(readOr(j, "resolutionHeight", defaults.resolutionHeight), {0, 720, 1080}, defaults.resolutionHeight);

        settings_.replaySeconds = oneOf(readOr(j, "replaySeconds", defaults.replaySeconds), {15, 30, 60, 120}, defaults.replaySeconds);
        settings_.clipFps = oneOf(readOr(j, "clipFps", defaults.clipFps), {30, 60, 120}, defaults.clipFps);
        settings_.clipQuality = clampInt(readOr(j, "clipQuality", defaults.clipQuality), 0, 3);

        settings_.captureWindow = readOr(j, "captureWindow", defaults.captureWindow);
        settings_.monitorDevice = fromUtf8(readOr<std::string>(j, "monitorDevice", ""));

        settings_.recordGameAudio = readOr(j, "recordGameAudio", defaults.recordGameAudio);
        settings_.recordMicAudio = readOr(j, "recordMicAudio", defaults.recordMicAudio);
        settings_.micDeviceId = fromUtf8(readOr<std::string>(j, "micDeviceId", ""));
        settings_.gameVolume = clampInt(readOr(j, "gameVolume", defaults.gameVolume), 0, 200);
        settings_.micVolume = clampInt(readOr(j, "micVolume", defaults.micVolume), 0, 200);

        settings_.recordingsFolder = fromUtf8(readOr<std::string>(j, "recordingsFolder", ""));
        settings_.clipsFolder = fromUtf8(readOr<std::string>(j, "clipsFolder", ""));

        // Lê como 64 bits para não truncar valores grandes antes de validar a faixa.
        auto validKey = [](std::uint64_t key) { return key >= 0x01 && key <= 0xFE; };
        const auto save = readOr<std::uint64_t>(j, "hotkeySave", defaults.hotkeySave);
        const auto record = readOr<std::uint64_t>(j, "hotkeyRecord", defaults.hotkeyRecord);
        const auto replayKey = readOr<std::uint64_t>(j, "hotkeyReplay", defaults.hotkeyReplay);
        if (validKey(save)) settings_.hotkeySave = static_cast<unsigned>(save);
        if (validKey(record)) settings_.hotkeyRecord = static_cast<unsigned>(record);
        if (validKey(replayKey)) settings_.hotkeyReplay = static_cast<unsigned>(replayKey);

        settings_.startWithWindows = readOr(j, "startWithWindows", defaults.startWithWindows);
    } catch (const nlohmann::json::exception&) {
        settings_ = Settings{};
        return false;
    }
    return true;
}

bool SettingsManager::save() const {
    std::error_code ec;
    std::filesystem::create_directories(appDataDir(), ec);

    nlohmann::json j = {
        {"recordFps", settings_.recordFps},
        {"recordQuality", settings_.recordQuality},
        {"resolutionHeight", settings_.resolutionHeight},
        {"replaySeconds", settings_.replaySeconds},
        {"clipFps", settings_.clipFps},
        {"clipQuality", settings_.clipQuality},
        {"captureWindow", settings_.captureWindow},
        {"monitorDevice", toUtf8(settings_.monitorDevice)},
        {"recordGameAudio", settings_.recordGameAudio},
        {"recordMicAudio", settings_.recordMicAudio},
        {"micDeviceId", toUtf8(settings_.micDeviceId)},
        {"gameVolume", settings_.gameVolume},
        {"micVolume", settings_.micVolume},
        {"recordingsFolder", toUtf8(settings_.recordingsFolder)},
        {"clipsFolder", toUtf8(settings_.clipsFolder)},
        {"hotkeySave", settings_.hotkeySave},
        {"hotkeyRecord", settings_.hotkeyRecord},
        {"hotkeyReplay", settings_.hotkeyReplay},
        {"startWithWindows", settings_.startWithWindows},
    };

    // Escreve em arquivo temporário e troca no final: evita JSON truncado se o app cair no meio.
    const auto target = settingsFile();
    auto tempPath = target;
    tempPath += L".tmp";
    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        out.close();
        // Só troca o arquivo se a escrita inteira (incluindo o flush do close) deu certo.
        if (out.fail()) {
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }
    std::filesystem::rename(tempPath, target, ec);
    return !ec;
}
