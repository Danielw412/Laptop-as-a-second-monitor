#pragma once
// Durable per-run diagnostics. The rolling logs in %TEMP%\LaptopMonitor are bounded and disposable; every run of
// the application with diagnostics on also gets a folder of its own that no later run deletes:
//
//   <root>\<run_id>\host.log      the same lines as the rolling host.log (numbered segments past 8 MB)
//   <root>\<run_id>\perf.jsonl    the same records as the rolling perf.jsonl (numbered segments past 32 MB)
//   <root>\<run_id>\session.json  what the run was and how it ended
//
// Portable on purpose (no Windows calls beyond the process id), so the lifecycle is unit tested.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
namespace lm {
/// Fixed for the life of the process. The run id names the archive folder and is stamped on every record.
struct RunIdentity {
    std::string runId;          // "20260918-142233-12345": local start time, then the process id
    uint32_t pid = 0;
    uint64_t processCreated = 0; // Windows process creation time (FILETIME ticks); tells a reused pid apart
    std::string startedAt;      // Local, "2026-09-18 14:22:33.123" like every record's "at"
    std::string startedAtUtc;   // "2026-09-18T12:22:33.123Z"
    std::chrono::system_clock::time_point started{};
};
/// Computed on first use. The start time is the process creation time (on Windows), whenever that first use is.
const RunIdentity &runIdentity();

/// Optional label for a controlled test run (--diagnostic-tag scrolling): 1-48 letters, digits, '.', '_', '-'.
bool validDiagnosticTag(std::string_view);
/// Throws std::invalid_argument for an invalid tag. Set once at startup, before any engine runs.
void setDiagnosticTag(std::string);
/// Empty when the run is untagged.
const std::string &diagnosticTag();

/// Process-wide generations. Every capture/encoder pipeline build takes the next pipeline session number and every
/// streaming engine the next engine session number, so (run_id, pipeline_session) names one pipeline lifetime -
/// the span over which the cumulative counters in perf.jsonl count up - even across engine restarts.
uint64_t nextPipelineSession();
uint64_t pipelineSessionCount();
uint64_t nextEngineSession();
uint64_t engineSessionCount();

/// Values session.json's "termination" takes.
namespace termination {
inline constexpr const char *kRunning = "running";             // Archive open (or the process died; see abnormal)
inline constexpr const char *kGraceful = "graceful";           // The application exited normally
inline constexpr const char *kDiagnosticsOff = "diagnostics_off"; // Logging was switched off; the run went on
inline constexpr const char *kWindowsSessionEnd = "windows_session_end"; // Sign-out or shutdown ended the run
inline constexpr const char *kUninstall = "uninstall";         // Closed so uninstall could delete it
inline constexpr const char *kFatal = "fatal";                 // Startup failed; see "fatal_error"
inline constexpr const char *kAbnormal = "abnormal";           // Found still "running" after its process was gone
} // namespace termination

class SessionArchive {
  public:
    /// root: the folder holding one subfolder per run.
    explicit SessionArchive(std::filesystem::path root) : root_(std::move(root)) {}
    /// Creates this run's folder, or reopens it when diagnostics were switched off and on again, and writes
    /// session.json as "running" with `metadata` merged in. Returns the folder.
    std::filesystem::path open(const nlohmann::json &metadata);
    /// Appends `entry` to the array `key` in session.json (settings changed mid-run, say). No-op when closed.
    void append(const std::string &key, nlohmann::json entry);
    /// Rewrites session.json with the end time, why the archive closed, and `facts` merged in. No-op when closed.
    void close(std::string_view why, const nlohmann::json &facts = nlohmann::json::object());
    bool isOpen() const {
        return open_;
    }
    const std::filesystem::path &root() const {
        return root_;
    }
    /// This run's folder once opened (it stays set after close, so it can still be shown).
    const std::filesystem::path &directory() const {
        return directory_;
    }
    /// Runs still marked "running" that are not this run and whose process `stillRunning` says is gone ended
    /// without closing their archive (crash, kill, power loss). Marks each "abnormal", with the time of the newest
    /// file in its folder as the end, and returns their run ids. Never deletes anything.
    static std::vector<std::string> reconcile(const std::filesystem::path &root,
                                              const std::function<bool(const nlohmann::json &)> &stillRunning);
    struct Summary {
        size_t runs = 0;
        uint64_t bytes = 0;
    };
    /// How many run folders exist under root and how much they hold, for the startup line.
    static Summary summarize(const std::filesystem::path &root);

  private:
    std::filesystem::path root_, directory_;
    nlohmann::json document_;
    bool open_ = false;
    void save() const;
};
/// Reads a session.json; a discarded value when missing, oversized or malformed.
nlohmann::json readSessionFile(const std::filesystem::path &file);
/// Whether the process that wrote a session.json is still alive: same pid and same creation time, so a pid that
/// Windows has since given to another process does not count. A process that exists but cannot be queried counts
/// as alive, so a run is never marked abnormal on a guess. Always false off Windows.
bool sessionProcessRunning(const nlohmann::json &session);
} // namespace lm
