#include "logging.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <system_error>
namespace lm {
std::string logTimestamp() {
    return logTimestamp(std::chrono::system_clock::now());
}
std::string logTimestamp(std::chrono::system_clock::time_point now) {
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
std::string printable(std::string_view text, size_t limit) {
    std::string out(text.substr(0, limit));
    for (auto &c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            c = '?';
    return out;
}
namespace {
// One append-only file with size-based rotation. The rolling logs replace a single <stem>.1<extension> backup;
// the archive keeps numbered segments <stem>.001<extension>, <stem>.002<extension>, ... and removes only the
// oldest one once more than `segments` exist, which bounds one run without ever touching another.
struct Stream {
    std::ofstream file;
    std::filesystem::path path;
    size_t written = 0;
    size_t limit = 2 * 1024 * 1024;
    unsigned segments = 0; // 0: single replaced backup
    unsigned next = 1;     // Number the next archive segment gets
    void start(const std::filesystem::path &directory, std::string_view name, size_t maximum, unsigned keep = 0) {
        close();
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        path = directory / std::string(name);
        limit = maximum;
        segments = keep;
        next = 1;
        if (segments) {
            // Reopening an archive (diagnostics turned off and on again) continues after its highest segment.
            const auto stem = path.stem().string() + ".", extension = path.extension().string();
            for (auto it = std::filesystem::directory_iterator(directory, ec);
                 !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                const auto found = it->path().filename().string();
                if (found.size() <= stem.size() + extension.size() || !found.starts_with(stem) ||
                    !found.ends_with(extension))
                    continue;
                const auto digits = found.substr(stem.size(), found.size() - stem.size() - extension.size());
                if (digits.size() <= 6 && std::all_of(digits.begin(), digits.end(),
                                                      [](char c) { return c >= '0' && c <= '9'; }))
                    next = std::max(next, unsigned(std::stoul(digits)) + 1);
            }
        }
        file.open(path, std::ios::app);
        written = open() ? size_t(std::filesystem::file_size(path, ec)) : 0;
    }
    std::filesystem::path segment(unsigned n) const {
        char number[16];
        std::snprintf(number, sizeof number, ".%03u", n);
        auto p = path;
        p.replace_extension(number + path.extension().string());
        return p;
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
            if (!segments) {
                auto rotated = path;
                rotated.replace_extension(".1" + path.extension().string());
                std::filesystem::rename(path, rotated, ec);
            } else {
                std::filesystem::rename(path, segment(next), ec);
                if (next > segments)
                    std::filesystem::remove(segment(next - segments), ec);
                ++next;
            }
            file.open(path, std::ios::trunc);
            written = 0;
        }
    }
};
} // namespace
struct Log::Impl {
    std::mutex mutex;
    Stream text, records;               // Rolling files in %TEMP%\LaptopMonitor
    Stream archiveText, archiveRecords; // This run's archive folder
    std::filesystem::path archive;
    uint64_t recordCount = 0;
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
void Log::openArchive(const std::filesystem::path &directory, const ArchiveLimits &limits) {
    std::lock_guard lock(impl_->mutex);
    // A segment count of 0 would mean "replace one backup"; the archive always keeps at least one segment.
    impl_->archiveText.start(directory, "host.log", limits.textSegmentBytes, std::max(1u, limits.textSegments));
    impl_->archiveRecords.start(directory, "perf.jsonl", limits.recordSegmentBytes,
                                std::max(1u, limits.recordSegments));
    impl_->archive = directory;
}
void Log::closeArchive() {
    std::lock_guard lock(impl_->mutex);
    impl_->archiveText.close();
    impl_->archiveRecords.close();
    impl_->archive.clear();
}
bool Log::recording() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->records.open() || impl_->archiveRecords.open();
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
std::filesystem::path Log::archiveDirectory() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->archive;
}
uint64_t Log::recordsWritten() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->recordCount;
}
void Log::write(LogLevel level, std::string_view message) {
    static const char *const names[] = {"debug", "info ", "warn ", "error"};
    std::string line = logTimestamp() + " [" + names[size_t(level)] + "] " + std::string(message);
    std::lock_guard lock(impl_->mutex);
    if (console_)
        (level >= LogLevel::Warning ? std::cerr : std::cout) << line << '\n';
    impl_->text.append(line);
    impl_->archiveText.append(line);
    if (impl_->sink)
        impl_->sink(level, line);
}
void Log::writeRecord(std::string_view json) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->records.open() && !impl_->archiveRecords.open())
        return;
    // The record's own fields are left untouched; the stamp is prepended so a line stands on its own.
    std::string line = "{\"at\":\"" + logTimestamp() + "\"";
    if (json.size() > 2 && json.front() == '{') {
        line += ',';
        line.append(json.substr(1));
    } else
        line += '}';
    impl_->records.append(line);
    impl_->archiveRecords.append(line);
    ++impl_->recordCount;
}
} // namespace lm
