#pragma once
// Thread-safe diagnostics log shared by every component. Never receives pairing codes or credentials.
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
namespace bm {
enum class LogLevel { Debug, Info, Warning, Error };
class Log {
  public:
    static Log &instance();
    /// Writes to <directory>/host.log, rotating to host.1.log at ~2 MB.
    void openFile(const std::filesystem::path &directory, std::string_view name = "host.log");
    void closeFile();
    void setConsole(bool on) {
        console_ = on;
    }
    /// Called with formatted lines; used by the GUI to keep a recent-lines buffer.
    void setSink(std::function<void(LogLevel, const std::string &)> sink);
    void write(LogLevel, std::string_view message);
    std::filesystem::path path() const;

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
} // namespace bm
