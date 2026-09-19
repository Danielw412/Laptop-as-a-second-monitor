#include "diagnostics.hpp"
#include "resources.hpp"
#include "session_archive.hpp"
namespace lm::app {
namespace {
SessionArchive &archive() {
    // An empty root makes open() fail, which startDiagnostics reports; the rolling logs still work.
    static SessionArchive instance([] {
        try {
            return sessionsDirectory();
        } catch (...) {
            return std::filesystem::path();
        }
    }());
    return instance;
}
nlohmann::json &launch() {
    static nlohmann::json value = nlohmann::json::object();
    return value;
}
// The settings a later reader needs to compare runs. A signaling URL can carry user:password@ in principle; that
// part never reaches the archive.
nlohmann::json settingsRecord(const Settings &s) {
    auto j = toJson(s);
    j.erase("version");
    auto url = s.signalingUrl;
    if (const auto scheme = url.find("://"); scheme != std::string::npos) {
        const auto at = url.find('@', scheme + 3), slash = url.find('/', scheme + 3);
        if (at != std::string::npos && (slash == std::string::npos || at < slash))
            url.erase(scheme + 3, at + 1 - (scheme + 3));
    }
    j["signalingUrl"] = url;
    return j;
}
nlohmann::json closingFacts(const nlohmann::json &extra) {
    nlohmann::json facts = {{"pipeline_sessions", pipelineSessionCount()},
                            {"engine_sessions", engineSessionCount()},
                            {"perf_records", Log::instance().recordsWritten()}};
    if (extra.is_object())
        for (auto &[key, value] : extra.items())
            facts[key] = value;
    return facts;
}
std::string megabytesText(uint64_t bytes) {
    return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB";
}
} // namespace
void rememberLaunch(nlohmann::json value) {
    launch() = std::move(value);
}
std::vector<std::pair<LogLevel, std::string>> startDiagnostics(const Settings &settings) {
    std::vector<std::pair<LogLevel, std::string>> notes;
    Log::instance().openFile(logDirectory());
    Log::instance().openRecordFile(logDirectory());
    auto &a = archive();
    if (a.isOpen())
        return notes;
    try {
        const bool first = a.directory().empty();
        if (first)
            for (auto &id : SessionArchive::reconcile(a.root(), sessionProcessRunning))
                notes.emplace_back(LogLevel::Warning, "Earlier run " + id +
                                                          " ended without closing its log (crash, forced exit or "
                                                          "power loss); its session.json now says \"abnormal\"");
        const auto directory = a.open(
            {{"machine", machineProfileJson()}, {"settings", settingsRecord(settings)}, {"launch", launch()}});
        Log::instance().openArchive(directory);
        const auto kept = SessionArchive::summarize(a.root());
        const auto &tag = diagnosticTag();
        notes.emplace_back(LogLevel::Info, "Session archive: " + directory.string() + " (run " +
                                               runIdentity().runId + (tag.empty() ? "" : ", tag " + tag) + ") | " +
                                               std::to_string(kept.runs) + " runs kept in " + a.root().string() +
                                               ", " + megabytesText(kept.bytes) + " in total");
    } catch (const std::exception &e) {
        // The rolling logs are already open; losing the archive must not cost the log as well.
        notes.emplace_back(LogLevel::Warning,
                           std::string("Session archive unavailable, logging to the rolling files only: ") +
                               e.what());
    }
    return notes;
}
void closeArchive(std::string_view why, const nlohmann::json &facts) {
    auto &a = archive();
    if (!a.isOpen())
        return;
    // Stop mirroring first so every line that made it to the archive is on disk before session.json says it ended.
    Log::instance().closeArchive();
    a.close(why, closingFacts(facts));
}
void stopDiagnostics(std::string_view why, const nlohmann::json &facts) {
    closeArchive(why, facts);
    Log::instance().closeFile();
    Log::instance().closeRecordFile();
}
void noteSettings(const Settings &settings) {
    // "settings" stays what the run started with; later changes are listed with when they happened.
    archive().append("settings_changes", {{"at", logTimestamp()}, {"settings", settingsRecord(settings)}});
}
std::filesystem::path archiveDirectory() {
    return archive().directory();
}
} // namespace lm::app
