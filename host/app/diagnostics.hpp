#pragma once
// Everything the diagnostics setting switches: the rolling logs in %TEMP%\LaptopMonitor and this run's archive
// under %LOCALAPPDATA%\LaptopMonitor\logs\sessions (see session_archive.hpp). UI thread only.
#include "logging.hpp"
#include "settings.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace lm::app {
/// How the process was started, for session.json. Call once at startup, before startDiagnostics().
void rememberLaunch(nlohmann::json launch);
/// Opens both log channels and the archive, and mirrors the channels into it. The first time in a run it also
/// marks earlier runs that died without closing their archive. Returns lines to log once the caller has written
/// its own header, so a run's log still starts with the version line.
std::vector<std::pair<LogLevel, std::string>> startDiagnostics(const Settings &);
/// Closes the archive with `why` recorded in session.json, then both rolling channels.
void stopDiagnostics(std::string_view why, const nlohmann::json &facts = nlohmann::json::object());
/// Closes only the archive (Windows is ending the session, or uninstall is about to delete it); the rolling logs
/// keep going. startDiagnostics() reopens the same folder.
void closeArchive(std::string_view why, const nlohmann::json &facts = nlohmann::json::object());
/// Records a settings change in session.json while the archive is open.
void noteSettings(const Settings &);
/// This run's archive folder; empty until diagnostics have been on at least once in this run.
std::filesystem::path archiveDirectory();
} // namespace lm::app
