#pragma once
// User settings (plain JSON) and the host credential (DPAPI). Settings are deliberately few: the things a daily user
// might change. Internal tuning stays in code.
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
namespace bm {
inline constexpr const char *kDefaultSignalingUrl = BM_DEFAULT_SIGNALING_URL;
inline constexpr const char *kViewerUrl = BM_VIEWER_URL;
enum class CaptureBackend { Auto, Wgc, Dxgi };
enum class QualityPreset { Efficient, Balanced, Quality };
struct Settings {
    bool startAtSignIn = false;
    bool autoStartDisplay = true;
    bool minimizeToTray = true;
    bool diagnosticsLog = true;
    CaptureBackend backend = CaptureBackend::Auto;
    unsigned fps = 60;
    QualityPreset quality = QualityPreset::Balanced;
    std::string signalingUrl = kDefaultSignalingUrl;
    bool operator==(const Settings &) const = default;
};
/// Initial and maximum encoder bitrate for a preset, in bits per second.
struct BitratePlan {
    uint32_t initial, minimum, maximum;
};
BitratePlan bitratePlan(QualityPreset);
const char *backendName(CaptureBackend);
const char *qualityName(QualityPreset);
nlohmann::json toJson(const Settings &);
/// Tolerant: unknown keys are ignored and invalid values fall back to defaults.
Settings settingsFromJson(const nlohmann::json &);
/// Clamps out-of-range values (fps, URL scheme) so a hand-edited file cannot break startup.
Settings sanitized(Settings);
class SettingsStore {
    std::filesystem::path path_;

  public:
    explicit SettingsStore(std::filesystem::path path) : path_(std::move(path)) {}
    const std::filesystem::path &path() const {
        return path_;
    }
    Settings load() const;
    void save(const Settings &) const;
};
#ifdef _WIN32
std::filesystem::path appDataDirectory(); // %LOCALAPPDATA%\BrowserMonitor
std::filesystem::path settingsPath();
std::filesystem::path credentialPath();
std::filesystem::path logDirectory();
/// Loads the DPAPI-protected host credential or creates a fresh random one. 64 lowercase hex characters.
std::string loadOrCreateHostSecret(const std::filesystem::path &);
#endif
/// Room id for a credential: first 32 hex characters of SHA-256 over the credential text.
std::string roomIdFor(const std::string &secret);
bool validSecret(const std::string &secret);
} // namespace bm
