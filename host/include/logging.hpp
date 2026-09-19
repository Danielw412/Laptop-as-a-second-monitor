#pragma once
// Thread-safe diagnostics log shared by every component. Never receives pairing codes or credentials.
//
// Two channels, both written only while the diagnostics setting is on:
//   host.log     human-readable lines: lifecycle, state changes, problems, and a periodic performance summary.
//   perf.jsonl   one JSON object per line: the full per-second performance sample, for analysis after the fact.
// The second channel exists so adding detailed measurements never makes the first one unreadable.
//
// Each channel has two destinations. The rolling files (%TEMP%\LaptopMonitor) are small and replace their one
// backup; the archive (this run's folder under %LOCALAPPDATA%\LaptopMonitor\logs\sessions) receives the same
// lines and keeps numbered segments, so a run can be read back after later runs have rotated the rolling files.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
namespace lm {
enum class LogLevel { Debug, Info, Warning, Error };
/// How much of one run the archive keeps: the live file plus up to this many full segments per channel. A perf
/// record is 4-6 KB a second while streaming, so the default holds roughly 12 hours of streaming.
struct ArchiveLimits {
    size_t textSegmentBytes = 8 * 1024 * 1024, recordSegmentBytes = 32 * 1024 * 1024;
    unsigned textSegments = 3, recordSegments = 7;
};
class Log {
  public:
    static Log &instance();
    /// Writes to <directory>/host.log, rotating to host.1.log at ~2 MB.
    void openFile(const std::filesystem::path &directory, std::string_view name = "host.log");
    void closeFile();
    /// Writes to <directory>/perf.jsonl, rotating to perf.1.jsonl at ~8 MB. One record per line.
    void openRecordFile(const std::filesystem::path &directory, std::string_view name = "perf.jsonl");
    void closeRecordFile();
    /// Mirrors both channels into <directory>/host.log and <directory>/perf.jsonl as well. There a full file
    /// becomes the next numbered segment (host.001.log, perf.001.jsonl, ...) and only segments beyond the
    /// per-run cap are removed; nothing outside the directory is touched. Reopening appends.
    void openArchive(const std::filesystem::path &directory, const ArchiveLimits &limits = {});
    void closeArchive();
    /// True while a record file is open: the caller can skip building a sample nobody would read.
    bool recording() const;
    void setConsole(bool on) {
        console_ = on;
    }
    /// Called with formatted lines; used by the GUI to keep a recent-lines buffer.
    void setSink(std::function<void(LogLevel, const std::string &)> sink);
    void write(LogLevel, std::string_view message);
    /// Appends one already-serialised JSON object, stamped with the wall-clock time it was written.
    void writeRecord(std::string_view json);
    std::filesystem::path path() const;
    std::filesystem::path recordPath() const;
    /// The archive folder while mirroring, otherwise empty.
    std::filesystem::path archiveDirectory() const;
    /// Performance records written since the process started (to either destination).
    uint64_t recordsWritten() const;

  private:
    struct Impl;
    Impl *impl_;
    bool console_ = false;
    Log();
};
/// Local wall-clock time as every log line and record carries it: "2026-09-17 17:58:05.454".
std::string logTimestamp();
std::string logTimestamp(std::chrono::system_clock::time_point);
/// Text from outside the process (a receiver's decoder name, say) made safe for one line of the readable log:
/// control characters become '?', and anything past `limit` bytes is cut off.
std::string printable(std::string_view text, size_t limit = 64);
inline void logDebug(std::string_view m) {
    Log::instance().write(LogLevel::Debug, m);
}
inline void logInfo(std::string_view m) {
    Log::instance().write(LogLevel::Info, m);
}
inline void logWarning(std::string_view m) {
    Log::instance().write(LogLevel::Warning, m);
}
inline void logError(std::string_view m) {
    Log::instance().write(LogLevel::Error, m);
}
inline void logRecord(std::string_view json) {
    Log::instance().writeRecord(json);
}
inline bool logRecording() {
    return Log::instance().recording();
}
} // namespace lm
