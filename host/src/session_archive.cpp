#include "session_archive.hpp"
#include "logging.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace lm {
namespace {
std::atomic<uint64_t> pipelineSessions{0}, engineSessions{0};
std::string &tagStorage() {
    static std::string tag;
    return tag;
}
std::string text(const nlohmann::json &j, const char *key) {
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}
// Written beside the target and renamed over it, so a crash mid-write leaves the previous session.json intact.
bool writeJson(const std::filesystem::path &file, const nlohmann::json &j) {
    auto temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << j.dump(2) << '\n';
        if (!out)
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    return !ec;
}
} // namespace
const RunIdentity &runIdentity() {
    static const RunIdentity identity = [] {
        RunIdentity r;
        auto now = std::chrono::system_clock::now();
#ifdef _WIN32
        r.pid = GetCurrentProcessId();
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            r.processCreated = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            // The run starts when the process did, however late this is first asked. FILETIME counts 100 ns from
            // 1601; the Unix epoch is 11644473600 s later.
            constexpr uint64_t kUnixEpoch = 116444736000000000ull;
            if (r.processCreated > kUnixEpoch)
                now = std::chrono::system_clock::time_point(
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::nanoseconds((r.processCreated - kUnixEpoch) * 100)));
        }
#else
        r.pid = uint32_t(getpid());
#endif
        const auto t = std::chrono::system_clock::to_time_t(now);
        const int ms =
            int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        std::tm local{}, utc{};
#ifdef _WIN32
        localtime_s(&local, &t);
        gmtime_s(&utc, &t);
#else
        localtime_r(&t, &local);
        gmtime_r(&t, &utc);
#endif
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%04d%02d%02d-%02d%02d%02d-%u", local.tm_year + 1900, local.tm_mon + 1,
                      local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, r.pid);
        r.runId = buffer;
        r.started = now;
        r.startedAt = logTimestamp(now);
        std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                      utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, ms);
        r.startedAtUtc = buffer;
        return r;
    }();
    return identity;
}
bool validDiagnosticTag(std::string_view tag) {
    return !tag.empty() && tag.size() <= 48 && std::all_of(tag.begin(), tag.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}
void setDiagnosticTag(std::string tag) {
    if (!tag.empty() && !validDiagnosticTag(tag))
        throw std::invalid_argument("A diagnostic tag is 1-48 letters, digits, '.', '_' or '-'");
    tagStorage() = std::move(tag);
}
const std::string &diagnosticTag() {
    return tagStorage();
}
uint64_t nextPipelineSession() {
    return ++pipelineSessions;
}
uint64_t pipelineSessionCount() {
    return pipelineSessions.load();
}
uint64_t nextEngineSession() {
    return ++engineSessions;
}
uint64_t engineSessionCount() {
    return engineSessions.load();
}
nlohmann::json readSessionFile(const std::filesystem::path &file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || std::filesystem::file_size(file, ec) > 256 * 1024)
        return nlohmann::json(nlohmann::json::value_t::discarded);
    std::ifstream in(file, std::ios::binary);
    return nlohmann::json::parse(in, nullptr, false);
}
bool sessionProcessRunning(const nlohmann::json &session) {
#ifdef _WIN32
    if (!session.is_object() || !session.contains("pid") || !session["pid"].is_number_unsigned())
        return false;
    const auto pid = session["pid"].get<uint32_t>();
    const uint64_t created =
        session.contains("process_created") && session["process_created"].is_number_unsigned()
            ? session["process_created"].get<uint64_t>()
            : 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    bool alive = false;
    DWORD code = 0;
    FILETIME creation{}, exited{}, kernel{}, user{};
    if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE &&
        GetProcessTimes(process, &creation, &exited, &kernel, &user))
        alive = !created || ((uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime) == created;
    CloseHandle(process);
    return alive;
#else
    (void)session;
    return false;
#endif
}
std::filesystem::path SessionArchive::open(const nlohmann::json &metadata) {
    const auto &id = runIdentity();
    if (!open_) {
        if (root_.empty())
            throw std::runtime_error("No location for the session archive");
        directory_ = root_ / id.runId;
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        if (ec || !std::filesystem::is_directory(directory_, ec))
            throw std::runtime_error("Cannot create the session archive " + directory_.string());
    }
    if (!document_.is_object())
        document_ = {{"schema", 1},
                     {"run_id", id.runId},
                     {"app", "Laptop Monitor"},
                     {"version", LM_VERSION},
                     {"pid", id.pid},
                     {"process_created", id.processCreated},
                     {"started_at", id.startedAt},
                     {"started_at_utc", id.startedAtUtc},
                     {"diagnostic_tag",
                      diagnosticTag().empty() ? nlohmann::json(nullptr) : nlohmann::json(diagnosticTag())},
                     {"diagnostics_periods", nlohmann::json::array()}};
    if (metadata.is_object())
        for (auto &[key, value] : metadata.items())
            document_[key] = value;
    if (!open_) {
        // Diagnostics can be switched off and on during a run; each stretch that was recorded is listed.
        document_["diagnostics_periods"].push_back({{"on", logTimestamp()}, {"off", nullptr}});
        document_["termination"] = termination::kRunning;
        document_["ended_at"] = nullptr;
        document_["duration_s"] = nullptr;
        open_ = true;
    }
    save();
    return directory_;
}
void SessionArchive::append(const std::string &key, nlohmann::json entry) {
    if (!open_)
        return;
    auto &list = document_[key];
    if (!list.is_array())
        list = nlohmann::json::array();
    list.push_back(std::move(entry));
    save();
}
void SessionArchive::close(std::string_view why, const nlohmann::json &facts) {
    if (!open_)
        return;
    const auto now = std::chrono::system_clock::now();
    if (facts.is_object())
        for (auto &[key, value] : facts.items())
            document_[key] = value;
    document_["termination"] = std::string(why);
    document_["ended_at"] = logTimestamp(now);
    // From process start, so it is the length of the run even when diagnostics were switched on late.
    const double seconds = std::chrono::duration<double>(now - runIdentity().started).count();
    document_["duration_s"] = std::max(0.0, std::round(seconds * 10) / 10);
    auto &periods = document_["diagnostics_periods"];
    if (periods.is_array() && !periods.empty())
        periods.back()["off"] = logTimestamp(now);
    open_ = false;
    save();
}
void SessionArchive::save() const {
    // Diagnostics never take the application down: a session.json that cannot be written is simply stale.
    try {
        writeJson(directory_ / "session.json", document_);
    } catch (...) {
    }
}
std::vector<std::string>
SessionArchive::reconcile(const std::filesystem::path &root,
                          const std::function<bool(const nlohmann::json &)> &stillRunning) {
    std::vector<std::string> ended;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code local;
        if (!it->is_directory(local))
            continue;
        const auto file = it->path() / "session.json";
        auto session = readSessionFile(file);
        if (!session.is_object() || text(session, "termination") != termination::kRunning)
            continue;
        const auto id = text(session, "run_id");
        if (id == runIdentity().runId || (stillRunning && stillRunning(session)))
            continue;
        // The newest write in the folder is the best available estimate of when that run actually stopped.
        std::optional<std::filesystem::file_time_type> newest;
        for (auto entry = std::filesystem::directory_iterator(it->path(), local);
             !local && entry != std::filesystem::directory_iterator(); entry.increment(local)) {
            std::error_code timeError;
            const auto written = entry->last_write_time(timeError);
            if (!timeError && (!newest || written > *newest))
                newest = written;
        }
        session["termination"] = termination::kAbnormal;
        session["ended_at"] = nullptr;
        if (newest) {
            const auto wall = std::chrono::system_clock::now() +
                              std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                  *newest - std::filesystem::file_time_type::clock::now());
            session["ended_at"] = logTimestamp(wall);
        }
        session["ended_at_source"] = "newest file in the folder";
        session["reconciled_at"] = logTimestamp();
        session["reconciled_by"] = runIdentity().runId;
        if (writeJson(file, session))
            ended.push_back(id.empty() ? it->path().filename().string() : id);
    }
    return ended;
}
SessionArchive::Summary SessionArchive::summarize(const std::filesystem::path &root) {
    Summary s;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code local;
        if (!it->is_directory(local))
            continue;
        ++s.runs;
        for (auto entry = std::filesystem::recursive_directory_iterator(it->path(), local);
             !local && entry != std::filesystem::recursive_directory_iterator(); entry.increment(local)) {
            std::error_code sizeError;
            if (entry->is_regular_file(sizeError)) {
                const auto size = entry->file_size(sizeError);
                if (!sizeError)
                    s.bytes += size;
            }
        }
    }
    return s;
}
} // namespace lm
