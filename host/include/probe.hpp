#pragma once
// Quality probes: the same coarse luma grid computed from the NV12 surface the encoder read and from the frame the
// receiver displayed, compared on the host. A probe says whether the receiver shows what was sent, and if not,
// whether the content itself is wrong (corruption: a stale or garbled band) or only its detail is gone
// (quantization: the rate control ran out of bits). Portable and header-only so the logic tests cover it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
namespace lm {
/// The probe grid: 8x8-pixel cells at 1080p. Calibrated on recorded streams (2026-09-22): at QP 50 (1.9 Mbps) 2.6-4.5%
/// of the detailed cells lose more than half their detail while at QP 37-44 almost none do, and cell means stay within
/// ~30 levels even at QP 50 (quantization keeps each block's DC). Coarser grids (60x60 or 16x16 cells) could not tell
/// QP 50 from QP 29 at all: stale text and current text have the same statistics at that size.
inline constexpr unsigned kProbeColumns = 240, kProbeRows = 135;
/// `mean` is the average luma of each cell (limited range, 16-235, as the encoder sees it); `detail` is the mean
/// absolute difference between horizontal neighbours, which collapses when a cell turns into flat blocks. Row-major,
/// one byte per value.
struct CellStats {
    unsigned columns = 0, rows = 0;
    std::vector<uint8_t> mean, detail;
    bool valid() const {
        return columns && rows && mean.size() == size_t(columns) * rows && detail.size() == mean.size();
    }
};
/// Grid statistics from an 8-bit luma plane with the given stride (bytes per row).
inline CellStats lumaCells(const uint8_t *plane, unsigned width, unsigned height, size_t stride,
                           unsigned columns = kProbeColumns, unsigned rows = kProbeRows) {
    CellStats s;
    if (!plane || !width || !height || !columns || !rows || columns > width || rows > height)
        return s;
    s.columns = columns;
    s.rows = rows;
    s.mean.resize(size_t(columns) * rows);
    s.detail.resize(s.mean.size());
    for (unsigned r = 0; r < rows; ++r) {
        const unsigned y0 = r * height / rows, y1 = (r + 1) * height / rows;
        for (unsigned c = 0; c < columns; ++c) {
            const unsigned x0 = c * width / columns, x1 = (c + 1) * width / columns;
            uint64_t sum = 0, gradient = 0, count = 0, pairs = 0;
            for (unsigned y = y0; y < y1; ++y) {
                const uint8_t *row = plane + size_t(y) * stride;
                for (unsigned x = x0; x < x1; ++x) {
                    sum += row[x];
                    ++count;
                    if (x + 1 < x1) {
                        gradient += unsigned(std::abs(int(row[x + 1]) - int(row[x])));
                        ++pairs;
                    }
                }
            }
            const size_t i = size_t(r) * columns + c;
            s.mean[i] = uint8_t(count ? (sum + count / 2) / count : 0);
            s.detail[i] = uint8_t(std::min<uint64_t>(255, pairs ? (gradient * 4 + pairs / 2) / pairs : 0));
        }
    }
    return s;
}
/// Limited-range BT.709 luma of an sRGB-ish pixel, the way the receiver's picture maps back onto the encoder's.
inline uint8_t lumaFromRgb(uint8_t r, uint8_t g, uint8_t b) {
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return uint8_t(std::clamp(16.0 + y * 219.0 / 255.0 + 0.5, 0.0, 255.0));
}
struct CellComparison {
    bool valid = false;
    double meanAbsDiff = 0, maxAbsDiff = 0, gain = 1, offset = 0;
    unsigned cells = 0;
    unsigned mismatchedCells = 0; // |mean difference| above the tolerance after the global fit
    unsigned detailedCells = 0;   // Cells where the source had real detail
    unsigned flattenedCells = 0;  // ...and where the received detail fell below half of it
    int firstBadRow = -1, lastBadRow = -1; // Grid rows holding mismatched cells: a horizontal band shows here
    /// "match"; "quantized" (content right, detail gone in at least 1% of the detailed cells: too few bits);
    /// "corrupted" (at least 0.5% of the cells, and at least 8, show other content: a stale, garbled or missing
    /// region); "unrelated" (most cells differ: a different frame, or no picture at all).
    std::string verdict() const {
        if (!valid)
            return "invalid";
        if (cells && mismatchedCells * 2 > cells)
            return "unrelated";
        if (mismatchedCells >= std::max(8u, cells / 200))
            return "corrupted";
        if (detailedCells && flattenedCells * 100 >= detailedCells)
            return "quantized";
        return "match";
    }
};
/// Compares the receiver's grid with the source's after fitting received = gain * source + offset over all cells,
/// which absorbs a different YUV->RGB matrix or range at the receiver. `tolerance` is in luma levels.
inline CellComparison compareCells(const CellStats &source, const CellStats &received, double tolerance = 24) {
    CellComparison c;
    if (!source.valid() || !received.valid() || source.columns != received.columns || source.rows != received.rows)
        return c;
    const size_t n = source.mean.size();
    // Robust fit: the median difference first (a damaged band must not drag the fit towards itself), then gain and
    // offset by least squares over the cells that agree with it.
    std::vector<double> differences(n);
    for (size_t i = 0; i < n; ++i)
        differences[i] = double(received.mean[i]) - double(source.mean[i]);
    std::nth_element(differences.begin(), differences.begin() + n / 2, differences.end());
    const double median = differences[n / 2];
    double sx = 0, sy = 0, sxx = 0, sxy = 0, used = 0;
    for (size_t i = 0; i < n; ++i) {
        const double x = source.mean[i], y = received.mean[i];
        if (std::abs(y - x - median) > tolerance * 2)
            continue;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++used;
    }
    const double variance = used ? sxx - sx * sx / used : 0;
    c.gain = 1;
    c.offset = median;
    if (used >= 8 && variance > used * 100) { // Enough cells, and enough spread in brightness to fit a slope
        c.gain = std::clamp((sxy - sx * sy / used) / variance, 0.8, 1.25);
        c.offset = (sy - c.gain * sx) / used;
    }
    c.valid = true;
    c.cells = unsigned(n);
    double total = 0;
    for (size_t i = 0; i < n; ++i) {
        const double diff = std::abs(double(received.mean[i]) - (c.gain * source.mean[i] + c.offset));
        total += diff;
        c.maxAbsDiff = std::max(c.maxAbsDiff, diff);
        const int row = int(i / source.columns);
        if (diff > tolerance) {
            ++c.mismatchedCells;
            if (c.firstBadRow < 0)
                c.firstBadRow = row;
            c.lastBadRow = row;
        }
        if (source.detail[i] >= 24) {
            ++c.detailedCells;
            if (received.detail[i] * 2 < source.detail[i])
                ++c.flattenedCells;
        }
    }
    c.meanAbsDiff = total / double(n);
    return c;
}
} // namespace lm
