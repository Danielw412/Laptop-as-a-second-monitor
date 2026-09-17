#pragma once
// Thread-safe diagnostics log shared by every component. Never receives pairing codes or credentials.
//
// Two channels, both written only while the diagnostics setting is on:
//   host.log     human-readable lines: lifecycle, state changes, problems, and a periodic performance summary.
//   perf.jsonl   one JSON object per line: the full per-second performance sample, for analysis after the fact.
// The second channel exists so adding detailed measurements never makes the first one unreadable.
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
namespace lm {
enum class LogLevel { Debug, Info, Warning, Error };
class Log {
  public:
    static Log &instance();
    /// Writes to <directory>/host.log, rotating to host.1.log at ~2 MB.
    void openFile(const std::filesystem::path &directory, std::string_view name = "host.log");
    void closeFile();
    /// Writes to <directory>/perf.jsonl, rotating to perf.1.jsonl at ~8 MB. One record per line.
    void openRecordFile(const std::filesystem::path &directory, std::string_view name = "perf.jsonl");
    void closeRecordFile();
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

  private:
    struct Impl;
    Impl *impl_;
    bool console_ = false;
    Log();
};
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
