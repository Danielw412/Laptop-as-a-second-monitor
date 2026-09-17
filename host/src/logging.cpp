#include "logging.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <system_error>
namespace lm {
namespace {
/// Wall-clock stamp shared by both channels. "2026-09-17 17:58:05.454".
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[40];
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms));
    return stamp;
}
// One append-only file with size-based rotation to <stem>.1<extension>. Both channels behave the same way; only
// the size they are allowed to reach differs.
struct Stream {
    std::ofstream file;
    std::filesystem::path path;
    size_t written = 0;
    size_t limit = 2 * 1024 * 1024;
    void start(const std::filesystem::path &directory, std::string_view name, size_t maximum) {
        close();
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        path = directory / std::string(name);
        limit = maximum;
        file.open(path, std::ios::app);
        written = open() ? size_t(std::filesystem::file_size(path, ec)) : 0;
    }
    void close() {
        if (file.is_open())
            file.close();
        file.clear(); // A closed stream still converts to true, so state is the wrong thing to test on.
    }
    bool open() const {
        return file.is_open() && file.good();
    }
    void append(const std::string &line) {
        if (!open())
            return;
        file << line << '\n';
        file.flush();
        written += line.size() + 1;
        if (written > limit) {
            file.close();
            std::error_code ec;
            auto rotated = path;
            rotated.replace_extension(".1" + path.extension().string());
            std::filesystem::rename(path, rotated, ec);
            file.open(path, std::ios::trunc);
            written = 0;
        }
    }
};
} // namespace
struct Log::Impl {
    std::mutex mutex;
    Stream text, records;
    std::function<void(LogLevel, const std::string &)> sink;
};
Log::Log() : impl_(new Impl) {}
Log &Log::instance() {
    static Log log;
    return log;
}
void Log::openFile(const std::filesystem::path &directory, std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    impl_->text.start(directory, name, 2 * 1024 * 1024);
}
void Log::closeFile() {
    std::lock_guard lock(impl_->mutex);
    impl_->text.close();
}
void Log::openRecordFile(const std::filesystem::path &directory, std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    // Performance records are one dense line a second, so they get a larger budget than the readable log.
    impl_->records.start(directory, name, 8 * 1024 * 1024);
}
void Log::closeRecordFile() {
    std::lock_guard lock(impl_->mutex);
    impl_->records.close();
}
bool Log::recording() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->records.open();
}
void Log::setSink(std::function<void(LogLevel, const std::string &)> sink) {
    std::lock_guard lock(impl_->mutex);
    impl_->sink = std::move(sink);
}
std::filesystem::path Log::path() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->text.path;
}
std::filesystem::path Log::recordPath() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->records.path;
}
void Log::write(LogLevel level, std::string_view message) {
    static const char *const names[] = {"debug", "info ", "warn ", "error"};
    std::string line = timestamp() + " [" + names[size_t(level)] + "] " + std::string(message);
    std::lock_guard lock(impl_->mutex);
    if (console_)
        (level >= LogLevel::Warning ? std::cerr : std::cout) << line << '\n';
    impl_->text.append(line);
    if (impl_->sink)
        impl_->sink(level, line);
}
void Log::writeRecord(std::string_view json) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->records.open())
        return;
    // The record's own fields are left untouched; the stamp is prepended so a line stands on its own.
    std::string line = "{\"at\":\"" + timestamp() + "\"";
    if (json.size() > 2 && json.front() == '{') {
        line += ',';
        line.append(json.substr(1));
    } else
        line += '}';
    impl_->records.append(line);
}
} // namespace lm
