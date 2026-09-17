#include "settings.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#endif
namespace lm {
BitratePlan bitratePlan(QualityPreset p) {
    switch (p) {
    case QualityPreset::Efficient:
        return {5000000, 1500000, 10000000};
    case QualityPreset::Quality:
        return {12000000, 2000000, 20000000};
    default:
        return {8000000, 1500000, 16000000};
    }
}
const char *backendName(CaptureBackend b) {
    switch (b) {
    case CaptureBackend::Wgc:
        return "wgc";
    case CaptureBackend::Dxgi:
        return "dxgi";
    default:
        return "auto";
    }
}
const char *qualityName(QualityPreset p) {
    switch (p) {
    case QualityPreset::Efficient:
        return "efficient";
    case QualityPreset::Quality:
        return "quality";
    default:
        return "balanced";
    }
}
nlohmann::json toJson(const Settings &s) {
    return {{"version", 1},
            {"startAtSignIn", s.startAtSignIn},
            {"autoStartDisplay", s.autoStartDisplay},
            {"minimizeToTray", s.minimizeToTray},
            {"diagnosticsLog", s.diagnosticsLog},
            {"captureBackend", backendName(s.backend)},
            {"fps", s.fps},
            {"quality", qualityName(s.quality)},
            {"displayScale", scaleName(s.displayScale)},
            {"signalingUrl", s.signalingUrl}};
}
Settings settingsFromJson(const nlohmann::json &j) {
    Settings s;
    if (!j.is_object())
        return s;
    auto boolean = [&](const char *key, bool &out) {
        if (j.contains(key) && j[key].is_boolean())
            out = j[key].get<bool>();
    };
    boolean("startAtSignIn", s.startAtSignIn);
    boolean("autoStartDisplay", s.autoStartDisplay);
    boolean("minimizeToTray", s.minimizeToTray);
    boolean("diagnosticsLog", s.diagnosticsLog);
    if (j.contains("captureBackend") && j["captureBackend"].is_string()) {
        auto v = j["captureBackend"].get<std::string>();
        s.backend = v == "wgc" ? CaptureBackend::Wgc : v == "dxgi" ? CaptureBackend::Dxgi : CaptureBackend::Auto;
    }
    if (j.contains("fps") && j["fps"].is_number_unsigned())
        s.fps = j["fps"].get<unsigned>();
    if (j.contains("quality") && j["quality"].is_string()) {
        auto v = j["quality"].get<std::string>();
        s.quality = v == "efficient" ? QualityPreset::Efficient
                    : v == "quality" ? QualityPreset::Quality
                                     : QualityPreset::Balanced;
    }
    if (j.contains("displayScale") && j["displayScale"].is_string())
        s.displayScale = scaleFromName(j["displayScale"].get<std::string>());
    if (j.contains("signalingUrl") && j["signalingUrl"].is_string())
        s.signalingUrl = j["signalingUrl"].get<std::string>();
    return sanitized(s);
}
Settings sanitized(Settings s) {
    if (s.fps != 30 && s.fps != 60)
        s.fps = s.fps < 45 ? 30 : 60;
    while (!s.signalingUrl.empty() && s.signalingUrl.back() == '/')
        s.signalingUrl.pop_back();
    const bool localhost = s.signalingUrl.starts_with("http://127.0.0.1") || s.signalingUrl.starts_with("http://localhost");
    if (!s.signalingUrl.starts_with("https://") && !localhost)
        s.signalingUrl = kDefaultSignalingUrl;
    if (s.signalingUrl.size() > 512)
        s.signalingUrl = kDefaultSignalingUrl;
    return s;
}
Settings SettingsStore::load() const {
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec))
        return Settings{};
    if (std::filesystem::file_size(path_, ec) > 65536)
        return Settings{};
    std::ifstream file(path_, std::ios::binary);
    auto parsed = nlohmann::json::parse(file, nullptr, false);
    if (parsed.is_discarded())
        return Settings{};
    return settingsFromJson(parsed);
}
void SettingsStore::save(const Settings &settings) const {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    auto temporary = path_;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("Cannot write settings");
        file << toJson(sanitized(settings)).dump(2) << '\n';
    }
    std::filesystem::rename(temporary, path_, ec);
    if (ec) {
        // Fall back to copy+remove on filesystems where rename over an existing file fails.
        std::filesystem::copy_file(temporary, path_, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(temporary);
        if (ec)
            throw std::runtime_error("Cannot save settings");
    }
}
bool validSecret(const std::string &secret) {
    return secret.size() == 64 &&
           std::all_of(secret.begin(), secret.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string roomIdFor(const std::string &secret) {
    return sha256Hex(secret).substr(0, 32);
}
#ifdef _WIN32
std::filesystem::path appDataDirectory() {
    wchar_t value[32768];
    auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", value, DWORD(std::size(value)));
    if (!size || size >= std::size(value))
        throw std::runtime_error("LOCALAPPDATA unavailable");
    return std::filesystem::path(value) / L"LaptopMonitor";
}
std::filesystem::path settingsPath() {
    return appDataDirectory() / L"settings.json";
}
std::filesystem::path credentialPath() {
    return appDataDirectory() / L"host.credential";
}
std::filesystem::path logDirectory() {
    // Diagnostics live in the user's Temp folder: they are disposable by definition, Windows already cleans the
    // folder, and it is the first place anyone looks for a log. Settings and the credential stay in LOCALAPPDATA.
    wchar_t value[MAX_PATH + 2]{};
    const auto size = GetTempPathW(DWORD(std::size(value)), value);
    if (size && size < std::size(value))
        return std::filesystem::path(value) / L"LaptopMonitor";
    return appDataDirectory() / L"logs"; // No usable TEMP: keep logging rather than lose it.
}
namespace {
std::string generateSecret() {
    std::vector<UCHAR> bytes(32);
    if (BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Secure random generator unavailable");
    return hexLower(bytes.data(), bytes.size());
}
void writeProtected(const std::filesystem::path &path, const std::string &plain) {
    std::string copy = plain;
    DATA_BLOB input{DWORD(copy.size()), reinterpret_cast<BYTE *>(copy.data())}, output{};
    if (!CryptProtectData(&input, L"Laptop Monitor host credential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot protect the host credential");
    SecureZeroMemory(copy.data(), copy.size());
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<char *>(output.pbData), output.cbData);
    }
    LocalFree(output.pbData);
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot save the host credential");
}
} // namespace
std::string loadOrCreateHostSecret(const std::filesystem::path &path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) <= 4096) {
        std::ifstream file(path, std::ios::binary);
        std::vector<BYTE> bytes((std::istreambuf_iterator<char>(file)), {});
        DATA_BLOB input{DWORD(bytes.size()), bytes.data()}, output{};
        if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            std::string secret(reinterpret_cast<char *>(output.pbData), output.cbData);
            SecureZeroMemory(output.pbData, output.cbData);
            LocalFree(output.pbData);
            if (validSecret(secret))
                return secret;
        }
        // Unreadable or malformed: a fresh credential simply means the receiver must pair again.
    }
    auto secret = generateSecret();
    writeProtected(path, secret);
    return secret;
}
#endif
} // namespace lm
