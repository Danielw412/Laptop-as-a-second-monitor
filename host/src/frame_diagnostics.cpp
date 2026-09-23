#include "frame_diagnostics.hpp"
#include <cstring>
namespace lm {
Readback::Readback(Device &device, size_t slots) : device_(device), slots_(std::max<size_t>(1, slots)) {}
size_t Readback::pending() const {
    return size_t(std::count_if(slots_.begin(), slots_.end(), [](const Slot &s) { return s.busy; }));
}
bool Readback::request(ID3D11Texture2D *source, uint64_t tag) {
    if (!source)
        return false;
    auto slot = std::find_if(slots_.begin(), slots_.end(), [](const Slot &s) { return !s.busy; });
    if (slot == slots_.end())
        return false;
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_NV12)
        return false;
    if (!slot->staging || slot->desc.Width != d.Width || slot->desc.Height != d.Height ||
        slot->desc.Format != d.Format) {
        D3D11_TEXTURE2D_DESC s{};
        s.Width = d.Width;
        s.Height = d.Height;
        s.MipLevels = s.ArraySize = s.SampleDesc.Count = 1;
        s.Format = d.Format;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        slot->staging.Reset();
        if (FAILED(device_.device->CreateTexture2D(&s, nullptr, &slot->staging)))
            return false;
        slot->desc = s;
    }
    device_.context->CopySubresourceRegion(slot->staging.Get(), 0, 0, 0, 0, source, 0, nullptr);
    slot->busy = true;
    slot->tag = tag;
    slot->issued = Clock::now();
    return true;
}
std::vector<CpuFrame> Readback::collect(bool wait) {
    std::vector<CpuFrame> out;
    for (auto &slot : slots_) {
        if (!slot.busy)
            continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = device_.context->Map(slot.staging.Get(), 0, D3D11_MAP_READ,
                                                wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            continue;
        slot.busy = false;
        if (FAILED(hr))
            continue;
        CpuFrame f;
        f.tag = slot.tag;
        f.format = slot.desc.Format;
        f.width = slot.desc.Width;
        f.height = slot.desc.Height;
        f.rowPitch = mapped.RowPitch;
        f.depthPitch = mapped.DepthPitch;
        f.mapWaitMs = std::chrono::duration<double, std::milli>(Clock::now() - slot.issued).count();
        const auto *base = static_cast<const uint8_t *>(mapped.pData);
        if (f.format == DXGI_FORMAT_NV12) {
            // The UV plane follows the Y plane at RowPitch * Height (the documented layout for planar staging maps).
            f.pixels.resize(size_t(f.width) * f.height * 3 / 2);
            for (unsigned y = 0; y < f.height; ++y)
                std::memcpy(f.pixels.data() + size_t(y) * f.width, base + size_t(y) * mapped.RowPitch, f.width);
            const auto *uv = base + size_t(mapped.RowPitch) * f.height;
            auto *outUv = f.pixels.data() + size_t(f.width) * f.height;
            for (unsigned y = 0; y < f.height / 2; ++y)
                std::memcpy(outUv + size_t(y) * f.width, uv + size_t(y) * mapped.RowPitch, f.width);
        } else {
            f.pixels.resize(size_t(f.width) * f.height * 4);
            for (unsigned y = 0; y < f.height; ++y)
                std::memcpy(f.pixels.data() + size_t(y) * f.width * 4, base + size_t(y) * mapped.RowPitch,
                            size_t(f.width) * 4);
        }
        device_.context->Unmap(slot.staging.Get(), 0);
        out.push_back(std::move(f));
    }
    return out;
}
CellStats lumaCells(const CpuFrame &f, unsigned columns, unsigned rows) {
    if (f.format == DXGI_FORMAT_NV12)
        return lumaCells(f.pixels.data(), f.width, f.height, f.width, columns, rows);
    if (f.format != DXGI_FORMAT_B8G8R8A8_UNORM || f.pixels.size() < size_t(f.width) * f.height * 4)
        return {};
    std::vector<uint8_t> luma(size_t(f.width) * f.height);
    for (size_t i = 0; i < luma.size(); ++i) {
        const uint8_t *p = f.pixels.data() + i * 4;
        luma[i] = lumaFromRgb(p[2], p[1], p[0]);
    }
    return lumaCells(luma.data(), f.width, f.height, f.width, columns, rows);
}
namespace {
uint8_t clampByte(double v) {
    return uint8_t(std::clamp(v + 0.5, 0.0, 255.0));
}
} // namespace
bool writeBmp(const std::filesystem::path &path, const CpuFrame &f) {
    if (!f.width || !f.height || f.pixels.empty())
        return false;
    const uint32_t rowBytes = (f.width * 3 + 3) & ~3u, imageBytes = rowBytes * f.height;
    std::vector<uint8_t> file(54 + size_t(imageBytes));
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(file.data() + at, &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(file.data() + at, &v, 2); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, uint32_t(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, f.width);
    put32(22, uint32_t(-int32_t(f.height))); // Top-down rows
    put16(26, 1);
    put16(28, 24);
    put32(34, imageBytes);
    for (unsigned y = 0; y < f.height; ++y) {
        uint8_t *out = file.data() + 54 + size_t(y) * rowBytes;
        for (unsigned x = 0; x < f.width; ++x) {
            uint8_t b, g, r;
            if (f.format == DXGI_FORMAT_NV12) {
                const double Y = f.pixels[size_t(y) * f.width + x];
                const uint8_t *uv = f.pixels.data() + size_t(f.width) * f.height + size_t(y / 2) * f.width + (x & ~1u);
                const double c = 1.164 * (Y - 16), u = uv[0] - 128.0, v = uv[1] - 128.0;
                r = clampByte(c + 1.793 * v);
                g = clampByte(c - 0.213 * u - 0.533 * v);
                b = clampByte(c + 2.112 * u);
            } else {
                const uint8_t *p = f.pixels.data() + (size_t(y) * f.width + x) * 4;
                b = p[0];
                g = p[1];
                r = p[2];
            }
            out[x * 3] = b;
            out[x * 3 + 1] = g;
            out[x * 3 + 2] = r;
        }
    }
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    return bool(o);
}
bool writeRaw(const std::filesystem::path &path, const CpuFrame &f, bool append) {
    std::ofstream o(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    o.write(reinterpret_cast<const char *>(f.pixels.data()), std::streamsize(f.pixels.size()));
    return bool(o);
}
BitstreamRecorder::BitstreamRecorder(const std::filesystem::path &h264Path) {
    std::error_code ec;
    if (h264Path.has_parent_path())
        std::filesystem::create_directories(h264Path.parent_path(), ec);
    stream_.open(h264Path, std::ios::binary | std::ios::trunc);
    auto index = h264Path;
    index.replace_extension(".jsonl");
    index_.open(index, std::ios::trunc);
}
void BitstreamRecorder::write(std::span<const uint8_t> bytes, const nlohmann::json &entry) {
    if (!open())
        return;
    stream_.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    auto line = entry;
    line["unit"] = units_++;
    index_ << line.dump() << '\n';
}
void FlightRecorder::push(std::span<const uint8_t> bytes, bool keyframe, nlohmann::json meta) {
    if (entries_.empty() && !keyframe)
        return; // A dump must start at a keyframe to decode on its own
    entries_.push_back({std::vector<uint8_t>(bytes.begin(), bytes.end()), std::move(meta), keyframe, Clock::now()});
    bytes_ += bytes.size();
    const auto now = Clock::now();
    // Trim whole keyframe intervals from the front while the second-oldest keyframe alone still covers the window.
    while (entries_.size() > 1 && (bytes_ > maxBytes_ || now - entries_.front().at > keep_)) {
        auto next = std::find_if(entries_.begin() + 1, entries_.end(), [](const Entry &e) { return e.keyframe; });
        if (next == entries_.end() || (bytes_ <= maxBytes_ && now - next->at < keep_))
            break;
        for (auto it = entries_.begin(); it != next; ++it)
            bytes_ -= it->bytes.size();
        entries_.erase(entries_.begin(), next);
    }
    if (bytes_ > maxBytes_ * 2)
        clear(); // No keyframe for far too long: start again at the next one rather than grow
}
size_t FlightRecorder::dump(const std::filesystem::path &directory) const {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    std::ofstream stream(directory / "stream.h264", std::ios::binary), index(directory / "stream.jsonl");
    if (!stream || !index)
        return 0;
    size_t unit = 0;
    for (auto &e : entries_) {
        stream.write(reinterpret_cast<const char *>(e.bytes.data()), std::streamsize(e.bytes.size()));
        auto line = e.meta;
        line["unit"] = unit++;
        index << line.dump() << '\n';
    }
    return unit;
}
} // namespace lm
