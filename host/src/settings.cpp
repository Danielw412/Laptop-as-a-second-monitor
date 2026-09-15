#include "settings.hpp"
#include <fstream>
#include <wincrypt.h>
namespace bm {
std::filesystem::path settingsPath() {
    wchar_t value[32768];
    auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", value, DWORD(std::size(value)));
    if (!size || size >= std::size(value)) throw std::runtime_error("LOCALAPPDATA unavailable");
    return std::filesystem::path(value) / L"BrowserMonitor" / L"settings.dpapi";
}
nlohmann::json loadSettings() {
    const auto path = settingsPath();
    if (!std::filesystem::exists(path)) return nlohmann::json::object();
    if (std::filesystem::file_size(path) > 16384) throw std::runtime_error("Invalid saved settings");
    std::ifstream file(path, std::ios::binary);
    std::vector<BYTE> bytes((std::istreambuf_iterator<char>(file)), {});
    DATA_BLOB input{DWORD(bytes.size()), bytes.data()}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot decrypt saved pairing for this Windows user");
    auto parsed = nlohmann::json::parse(output.pbData, output.pbData + output.cbData, nullptr, false);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    if (!parsed.is_object()) throw std::runtime_error("Invalid saved settings");
    return parsed;
}
void saveSettings(const nlohmann::json &settings) {
    auto plain = settings.dump();
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE *>(plain.data())}, output{};
    if (!CryptProtectData(&input, L"Browser Monitor pairing", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot protect saved pairing");
    SecureZeroMemory(plain.data(), plain.size());
    auto path = settingsPath(), temporary = path;
    temporary += L".tmp";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<char *>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot save pairing settings");
}
std::string displayIdentity(const Display &display) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return {};
    for (UINT32 i = 0; i < pathCount; ++i) {
        const auto &p = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), p.sourceInfo.adapterId, p.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || utf8(source.viewGdiDeviceName) != display.name) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), p.targetInfo.adapterId, p.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) return utf8(target.monitorDevicePath);
    }
    return {};
}
}
