#pragma once
// Diagnostic copies of pipeline data, for telling where a damaged picture came from: the captured frame, the NV12
// surface the encoder read, the bitstream it wrote, or what the receiver finally showed. None of this is on the
// normal video path. Surface copies are taken only for bench recordings, the occasional quality probe and
// snapshots a receiver asks for, and they are asynchronous: the GPU copy is queued now and mapped a few frames later
// without waiting, so a probe never stalls the frame loop.
#include "platform.hpp"
#include "probe.hpp"
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
namespace lm {
/// A surface on the CPU, tightly packed. BGRA: width * 4 bytes per row. NV12: `height` rows of Y then `height / 2`
/// rows of interleaved UV, `width` bytes each. rowPitch/depthPitch are what the driver reported for the mapped
/// staging copy, kept so a stride problem is visible in the record rather than silently repacked away.
struct CpuFrame {
    uint64_t tag = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0, height = 0, rowPitch = 0, depthPitch = 0;
    std::vector<uint8_t> pixels;
    double mapWaitMs = 0; // From the copy being queued to the map succeeding
};
class Readback {
    struct Slot {
        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC desc{};
        bool busy = false;
        uint64_t tag = 0;
        Clock::time_point issued{};
    };
    Device &device_;
    std::vector<Slot> slots_;

  public:
    explicit Readback(Device &, size_t slots = 3);
    /// Queues a GPU copy of `source` (BGRA or NV12). False when every staging copy is still waiting to be read.
    bool request(ID3D11Texture2D *source, uint64_t tag);
    /// Maps the copies the GPU has finished. With `wait`, blocks for all of them (bench teardown only).
    std::vector<CpuFrame> collect(bool wait = false);
    size_t pending() const;
};
/// Grid statistics of a CPU frame's luma: NV12's Y plane directly, BGRA through lumaFromRgb.
CellStats lumaCells(const CpuFrame &, unsigned columns = kProbeColumns, unsigned rows = kProbeRows);
/// Standard uncompressed BMP (BGRA as is, NV12 converted with BT.709 limited range) for looking at a frame.
bool writeBmp(const std::filesystem::path &, const CpuFrame &);
/// Raw planes exactly as packed in CpuFrame (NV12 or BGRA), for ffmpeg -f rawvideo.
bool writeRaw(const std::filesystem::path &, const CpuFrame &, bool append = false);
/// Annex B elementary stream of exactly the access units handed to the network, plus one JSON line per unit, so the
/// stream a lossless receiver would have decoded can be replayed offline (ffmpeg -i stream.h264).
class BitstreamRecorder {
    std::ofstream stream_, index_;
    uint64_t units_ = 0;

  public:
    explicit BitstreamRecorder(const std::filesystem::path &h264Path);
    bool open() const {
        return stream_.is_open() && index_.is_open();
    }
    void write(std::span<const uint8_t> bytes, const nlohmann::json &entry);
    uint64_t units() const {
        return units_;
    }
};
/// The last few seconds of the stream in memory, always starting at a keyframe so a dump decodes on its own. Written
/// out when a receiver reports a damaged picture; bounded in bytes and time so it never grows with the session.
class FlightRecorder {
    struct Entry {
        std::vector<uint8_t> bytes;
        nlohmann::json meta;
        bool keyframe;
        Clock::time_point at;
    };
    std::deque<Entry> entries_;
    size_t bytes_ = 0, maxBytes_;
    std::chrono::seconds keep_;

  public:
    explicit FlightRecorder(size_t maxBytes = 24 * 1024 * 1024, std::chrono::seconds keep = std::chrono::seconds(20))
        : maxBytes_(maxBytes), keep_(keep) {}
    void push(std::span<const uint8_t> bytes, bool keyframe, nlohmann::json meta);
    void clear() {
        entries_.clear();
        bytes_ = 0;
    }
    size_t units() const {
        return entries_.size();
    }
    size_t bytes() const {
        return bytes_;
    }
    /// Writes stream.h264 and stream.jsonl into `directory`. Returns the number of access units written.
    size_t dump(const std::filesystem::path &directory) const;
};
} // namespace lm
