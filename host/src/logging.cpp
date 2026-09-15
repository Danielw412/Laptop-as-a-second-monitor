#include "logging.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <system_error>
namespace bm {
struct Log::Impl {
    std::mutex mutex;
    std::ofstream file;
    std::filesystem::path path;
    std::function<void(LogLevel, const std::string &)> sink;
    size_t written = 0;
};
Log::Log() : impl_(new Impl) {}
Log &Log::instance() {
    static Log log;
    return log;
}
void Log::openFile(const std::filesystem::path &directory, std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    impl_->path = directory / std::string(name);
    impl_->file.open(impl_->path, std::ios::app);
    impl_->written = impl_->file ? size_t(std::filesystem::file_size(impl_->path, ec)) : 0;
}
void Log::closeFile() {
    std::lock_guard lock(impl_->mutex);
    impl_->file.close();
}
void Log::setSink(std::function<void(LogLevel, const std::string &)> sink) {
    std::lock_guard lock(impl_->mutex);
    impl_->sink = std::move(sink);
}
std::filesystem::path Log::path() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->path;
}
void Log::write(LogLevel level, std::string_view message) {
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
    static const char *const names[] = {"debug", "info ", "warn ", "error"};
    std::string line = std::string(stamp) + " [" + names[size_t(level)] + "] " + std::string(message);
    std::lock_guard lock(impl_->mutex);
    if (console_)
        (level >= LogLevel::Warning ? std::cerr : std::cout) << line << '\n';
    if (impl_->file) {
        impl_->file << line << '\n';
        impl_->file.flush();
        impl_->written += line.size() + 1;
        if (impl_->written > 2 * 1024 * 1024) {
            impl_->file.close();
            std::error_code ec;
            auto rotated = impl_->path;
            rotated.replace_extension(".1.log");
            std::filesystem::rename(impl_->path, rotated, ec);
            impl_->file.open(impl_->path, std::ios::trunc);
            impl_->written = 0;
        }
    }
    if (impl_->sink)
        impl_->sink(level, line);
}
} // namespace bm
