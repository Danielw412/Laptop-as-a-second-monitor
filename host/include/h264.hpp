#pragma once
// Read-only inspection of the H.264 (Annex B) access units the encoder produces: NAL unit types, the parameter sets,
// and the slice header fields the host logs per frame (slice QP, slice type, frame_num, IDR). It exists to answer
// two questions from the stream itself rather than from the encoder's settings: how coarse was this frame
// (the quantizer), and is the reference chain the receiver decodes against still intact (frame_num continuity).
// Reporting only: nothing here modifies the stream, and a field it cannot parse is left empty, never guessed.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace lm::h264 {
enum NalType : unsigned {
    kSlice = 1,
    kIdr = 5,
    kSei = 6,
    kSps = 7,
    kPps = 8,
    kAud = 9,
};
/// Exp-Golomb reader over one NAL unit's payload with emulation prevention bytes (00 00 03) removed. Reading past
/// the end sets a flag instead of throwing; callers check ok() before trusting anything they read.
class BitReader {
    std::vector<uint8_t> rbsp_;
    size_t bit_ = 0;
    bool overrun_ = false;

  public:
    explicit BitReader(std::span<const uint8_t> payload) {
        rbsp_.reserve(payload.size());
        unsigned zeros = 0;
        for (auto byte : payload) {
            if (zeros >= 2 && byte == 3) {
                zeros = 0;
                continue;
            }
            zeros = byte == 0 ? zeros + 1 : 0;
            rbsp_.push_back(byte);
        }
    }
    bool ok() const {
        return !overrun_;
    }
    uint32_t bits(unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (bit_ >= rbsp_.size() * 8) {
                overrun_ = true;
                return 0;
            }
            v = (v << 1) | ((rbsp_[bit_ / 8] >> (7 - bit_ % 8)) & 1u);
            ++bit_;
        }
        return v;
    }
    bool flag() {
        return bits(1) != 0;
    }
    uint32_t ue() {
        unsigned zeros = 0;
        while (!flag()) {
            if (overrun_ || ++zeros > 31) {
                overrun_ = true;
                return 0;
            }
        }
        if (!zeros)
            return 0;
        return uint32_t((uint64_t(1) << zeros) - 1 + bits(zeros));
    }
    int32_t se() {
        const uint32_t k = ue();
        return k & 1 ? int32_t((k + 1) / 2) : -int32_t(k / 2);
    }
};
struct Sps {
    bool valid = false;
    unsigned id = 0, profile = 0, level = 0, chromaFormat = 1;
    bool separateColourPlane = false;
    unsigned log2MaxFrameNum = 4, pocType = 0, log2MaxPocLsb = 4;
    bool deltaPocAlwaysZero = false;
    unsigned maxRefFrames = 0;
    bool gapsAllowed = false, frameMbsOnly = true;
    unsigned width = 0, height = 0; // Cropped picture size
    // Colour signalling from the VUI, when present. Without it a receiver picks its own default matrix and range.
    std::optional<bool> fullRange;
    std::optional<unsigned> matrix;
};
struct Pps {
    bool valid = false;
    unsigned id = 0, spsId = 0;
    bool cabac = false, bottomFieldPicOrder = false;
    unsigned refIdxL0 = 1, refIdxL1 = 1;
    bool weightedPred = false;
    unsigned weightedBipred = 0;
    int initQp = 26;
    bool deblockingControl = false, redundantPicCnt = false;
};
/// One encoded frame as the stream describes it.
struct AccessUnit {
    unsigned nalUnits = 0, slices = 0;
    bool idr = false, sps = false, pps = false, reference = false;
    std::optional<unsigned> sliceType; // 0 P, 1 B, 2 I (of the first slice)
    std::optional<unsigned> frameNum, maxFrameNum;
    std::optional<int> qpMin, qpMax; // Slice QP (26 + pic_init_qp_minus26 + slice_qp_delta) over the slices
    double qpMean = 0;
    unsigned deblockingDisabledSlices = 0;
    unsigned unparsedSlices = 0; // Slices whose header could not be read (no parameter set, malformed)
};
namespace detail {
inline void skipScalingList(BitReader &r, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0)
            next = (last + r.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}
} // namespace detail
/// Keeps the parameter sets seen so far (they arrive with keyframes) and summarises each access unit.
class Parser {
    std::array<Sps, 32> sps_{};
    std::array<Pps, 256> pps_{};

    static std::optional<Sps> parseSps(std::span<const uint8_t> payload) {
        BitReader r(payload);
        Sps s;
        s.profile = r.bits(8);
        r.bits(8); // constraint flags
        s.level = r.bits(8);
        s.id = r.ue();
        switch (s.profile) {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138:
        case 139: case 134: case 135:
            s.chromaFormat = r.ue();
            if (s.chromaFormat == 3)
                s.separateColourPlane = r.flag();
            r.ue(); // bit_depth_luma_minus8
            r.ue(); // bit_depth_chroma_minus8
            r.flag(); // qpprime_y_zero_transform_bypass_flag
            if (r.flag())
                for (int i = 0; i < (s.chromaFormat != 3 ? 8 : 12); ++i)
                    if (r.flag())
                        detail::skipScalingList(r, i < 6 ? 16 : 64);
            break;
        default:
            break;
        }
        s.log2MaxFrameNum = r.ue() + 4;
        s.pocType = r.ue();
        if (s.pocType == 0)
            s.log2MaxPocLsb = r.ue() + 4;
        else if (s.pocType == 1) {
            s.deltaPocAlwaysZero = r.flag();
            r.se();
            r.se();
            const auto cycle = r.ue();
            for (uint32_t i = 0; i < cycle && i < 256 && r.ok(); ++i)
                r.se();
        }
        s.maxRefFrames = r.ue();
        s.gapsAllowed = r.flag();
        const unsigned widthMbs = r.ue() + 1, heightMapUnits = r.ue() + 1;
        s.frameMbsOnly = r.flag();
        if (!s.frameMbsOnly)
            r.flag(); // mb_adaptive_frame_field_flag
        r.flag();     // direct_8x8_inference_flag
        unsigned cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
        if (r.flag()) {
            cropLeft = r.ue();
            cropRight = r.ue();
            cropTop = r.ue();
            cropBottom = r.ue();
        }
        const unsigned cropX = s.chromaFormat == 1 || s.chromaFormat == 2 ? 2 : 1;
        const unsigned cropY = (s.chromaFormat == 1 ? 2 : 1) * (2 - (s.frameMbsOnly ? 1 : 0));
        s.width = widthMbs * 16 - cropX * (cropLeft + cropRight);
        s.height = (2 - (s.frameMbsOnly ? 1 : 0)) * heightMapUnits * 16 - cropY * (cropTop + cropBottom);
        if (!r.ok() || s.id >= 32 || s.log2MaxFrameNum > 16 || s.log2MaxPocLsb > 16)
            return std::nullopt;
        s.valid = true;
        if (r.flag()) { // vui_parameters_present_flag: read as far as the colour description
            if (r.flag() && r.bits(8) == 255) {
                r.bits(16);
                r.bits(16);
            }
            if (r.flag())
                r.flag();
            if (r.flag()) {
                r.bits(3);
                const bool full = r.flag();
                std::optional<unsigned> matrix;
                if (r.flag()) {
                    r.bits(8);
                    r.bits(8);
                    matrix = r.bits(8);
                }
                if (r.ok()) {
                    s.fullRange = full;
                    s.matrix = matrix;
                }
            }
        }
        return s;
    }
    static std::optional<Pps> parsePps(std::span<const uint8_t> payload) {
        BitReader r(payload);
        Pps p;
        p.id = r.ue();
        p.spsId = r.ue();
        p.cabac = r.flag();
        p.bottomFieldPicOrder = r.flag();
        const auto groups = r.ue();
        if (groups > 0) {
            const auto type = r.ue();
            if (type == 0)
                for (uint32_t i = 0; i <= groups && r.ok(); ++i)
                    r.ue();
            else if (type == 2)
                for (uint32_t i = 0; i < groups && r.ok(); ++i) {
                    r.ue();
                    r.ue();
                }
            else if (type >= 3 && type <= 5) {
                r.flag();
                r.ue();
            } else if (type == 6) {
                const auto units = r.ue() + 1;
                unsigned bits = 0;
                while ((1u << bits) < groups + 1)
                    ++bits;
                for (uint32_t i = 0; i < units && r.ok(); ++i)
                    r.bits(bits);
            }
        }
        p.refIdxL0 = r.ue() + 1;
        p.refIdxL1 = r.ue() + 1;
        p.weightedPred = r.flag();
        p.weightedBipred = r.bits(2);
        p.initQp = 26 + r.se();
        r.se(); // pic_init_qs_minus26
        r.se(); // chroma_qp_index_offset
        p.deblockingControl = r.flag();
        r.flag(); // constrained_intra_pred_flag
        p.redundantPicCnt = r.flag();
        if (!r.ok() || p.id >= 256 || p.spsId >= 32)
            return std::nullopt;
        p.valid = true;
        return p;
    }
    struct SliceHeader {
        unsigned type = 0, frameNum = 0, maxFrameNum = 0;
        int qp = 0;
        bool deblockingDisabled = false;
    };
    std::optional<SliceHeader> parseSlice(std::span<const uint8_t> payload, unsigned nalType, unsigned refIdc) const {
        BitReader r(payload);
        SliceHeader h;
        r.ue(); // first_mb_in_slice
        h.type = r.ue() % 5;
        const auto ppsId = r.ue();
        if (!r.ok() || ppsId >= pps_.size() || !pps_[ppsId].valid)
            return std::nullopt;
        const auto &p = pps_[ppsId];
        const auto &s = sps_[p.spsId];
        if (!s.valid)
            return std::nullopt;
        if (s.separateColourPlane)
            r.bits(2);
        h.frameNum = r.bits(s.log2MaxFrameNum);
        h.maxFrameNum = 1u << s.log2MaxFrameNum;
        bool field = false;
        if (!s.frameMbsOnly && (field = r.flag()))
            r.flag();
        if (nalType == kIdr)
            r.ue();
        if (s.pocType == 0) {
            r.bits(s.log2MaxPocLsb);
            if (p.bottomFieldPicOrder && !field)
                r.se();
        }
        if (s.pocType == 1 && !s.deltaPocAlwaysZero) {
            r.se();
            if (p.bottomFieldPicOrder && !field)
                r.se();
        }
        if (p.redundantPicCnt)
            r.ue();
        const bool b = h.type == 1, predicted = h.type == 0 || h.type == 3 || b;
        if (b)
            r.flag();
        unsigned refL0 = p.refIdxL0, refL1 = p.refIdxL1;
        if (predicted && r.flag()) {
            refL0 = r.ue() + 1;
            if (b)
                refL1 = r.ue() + 1;
        }
        auto modifications = [&] {
            if (!r.flag())
                return;
            for (int guard = 0; guard < 64 && r.ok(); ++guard) {
                const auto idc = r.ue();
                if (idc == 3)
                    break;
                r.ue();
            }
        };
        if (h.type != 2 && h.type != 4)
            modifications();
        if (b)
            modifications();
        if ((p.weightedPred && (h.type == 0 || h.type == 3)) || (p.weightedBipred == 1 && b)) {
            r.ue();
            const bool chroma = s.chromaFormat != 0 && !s.separateColourPlane;
            if (chroma)
                r.ue();
            auto table = [&](unsigned count) {
                for (unsigned i = 0; i < count && i < 32 && r.ok(); ++i) {
                    if (r.flag()) {
                        r.se();
                        r.se();
                    }
                    if (chroma && r.flag())
                        for (int j = 0; j < 4; ++j)
                            r.se();
                }
            };
            table(refL0);
            if (b)
                table(refL1);
        }
        if (refIdc) {
            if (nalType == kIdr) {
                r.flag();
                r.flag();
            } else if (r.flag()) {
                for (int guard = 0; guard < 64 && r.ok(); ++guard) {
                    const auto op = r.ue();
                    if (op == 0)
                        break;
                    if (op == 1 || op == 3)
                        r.ue();
                    if (op == 2)
                        r.ue();
                    if (op == 3 || op == 6)
                        r.ue();
                    if (op == 4)
                        r.ue();
                }
            }
        }
        if (p.cabac && h.type != 2 && h.type != 4)
            r.ue();
        h.qp = p.initQp + r.se();
        if (h.type == 3 || h.type == 4) {
            if (h.type == 3)
                r.flag();
            r.se();
        }
        if (p.deblockingControl)
            h.deblockingDisabled = r.ue() == 1;
        if (!r.ok() || h.qp < 0 || h.qp > 51)
            return std::nullopt;
        return h;
    }

  public:
    /// Splits an Annex B buffer into NAL unit payloads (start codes removed), in order.
    static std::vector<std::span<const uint8_t>> units(std::span<const uint8_t> annexB) {
        std::vector<std::span<const uint8_t>> out;
        constexpr size_t none = ~size_t(0);
        size_t i = 0, start = none;
        auto startCodeAt = [&](size_t at) {
            return at + 2 < annexB.size() && annexB[at] == 0 && annexB[at + 1] == 0 && annexB[at + 2] == 1;
        };
        while (i < annexB.size()) {
            if (startCodeAt(i)) {
                if (start != none) {
                    size_t end = i;
                    while (end > start && annexB[end - 1] == 0) // A 4-byte start code's leading zero
                        --end;
                    out.push_back(annexB.subspan(start, end - start));
                }
                i += 3;
                start = i;
            } else
                ++i;
        }
        if (start != none && start < annexB.size())
            out.push_back(annexB.subspan(start));
        return out;
    }
    const Sps *activeSps() const {
        for (auto &s : sps_)
            if (s.valid)
                return &s;
        return nullptr;
    }
    AccessUnit parse(std::span<const uint8_t> annexB) {
        AccessUnit au;
        double qpSum = 0;
        for (auto nal : units(annexB)) {
            if (nal.empty())
                continue;
            ++au.nalUnits;
            const unsigned type = nal[0] & 0x1f, refIdc = (nal[0] >> 5) & 3;
            const auto payload = nal.subspan(1);
            if (type == kSps) {
                au.sps = true;
                if (auto s = parseSps(payload))
                    sps_[s->id] = *s;
            } else if (type == kPps) {
                au.pps = true;
                if (auto p = parsePps(payload))
                    pps_[p->id] = *p;
            } else if (type == kSlice || type == kIdr) {
                ++au.slices;
                au.idr = au.idr || type == kIdr;
                au.reference = au.reference || refIdc != 0;
                // The header is in the first few dozen bytes; copying the whole slice (tens of KB per frame at
                // 16 Mbps) to read it would cost the engine thread for nothing.
                if (auto h = parseSlice(payload.first(std::min<size_t>(payload.size(), 128)), type, refIdc)) {
                    if (!au.sliceType)
                        au.sliceType = h->type;
                    if (!au.frameNum) {
                        au.frameNum = h->frameNum;
                        au.maxFrameNum = h->maxFrameNum;
                    }
                    au.qpMin = au.qpMin ? std::min(*au.qpMin, h->qp) : h->qp;
                    au.qpMax = au.qpMax ? std::max(*au.qpMax, h->qp) : h->qp;
                    qpSum += h->qp;
                    if (h->deblockingDisabled)
                        ++au.deblockingDisabledSlices;
                } else
                    ++au.unparsedSlices;
            }
        }
        const unsigned parsed = au.slices - au.unparsedSlices;
        au.qpMean = parsed ? qpSum / parsed : 0;
        return au;
    }
};
/// Follows frame_num across the frames actually handed to the receiver. A gap means the receiver is about to decode
/// a frame whose reference it never got: the picture smears until the next IDR. The encoder never produces one on
/// its own, so a gap here is always a frame lost between the encoder and the network.
class ReferenceChain {
    std::optional<unsigned> previousReference_;

  public:
    /// Returns false when this frame does not follow the last reference frame. An IDR always restarts the chain.
    bool follows(const AccessUnit &au) {
        if (!au.frameNum || !au.maxFrameNum || !au.slices)
            return true;
        bool ok = true;
        if (au.idr)
            previousReference_.reset();
        else if (previousReference_) {
            const unsigned expected = (*previousReference_ + 1) % *au.maxFrameNum;
            ok = *au.frameNum == expected || *au.frameNum == *previousReference_;
        }
        if (au.reference)
            previousReference_ = *au.frameNum;
        return ok;
    }
    void reset() {
        previousReference_.reset();
    }
};
} // namespace lm::h264
