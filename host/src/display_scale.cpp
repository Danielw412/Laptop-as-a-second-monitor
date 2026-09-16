#include "display_scale.hpp"
#include "logging.hpp"
#include <algorithm>
#include <array>
#include <windows.h>
namespace lm {
namespace {
// Windows exposes per-monitor scaling through two DISPLAYCONFIG_DEVICE_INFO_TYPE values that are not in the SDK
// headers. They are what the Settings scaling slider itself writes, they have behaved the same way since Windows
// 8.1, and they are addressed per source path, so nothing outside the named display can be affected. If a future
// build stops answering, every call below fails cleanly and the caller tells the user to use Settings.
constexpr int kGetDpiScaling = -3;
constexpr int kSetDpiScaling = -4;
struct DpiScaleGet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t minimumRelative; // Steps below the recommended scale that this display allows
    int32_t currentRelative; // Where it sits now, relative to the recommended scale
    int32_t maximumRelative;
};
struct DpiScaleSet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    int32_t relative;
};
/// The scaling ladder Windows offers, in order. A "relative" value indexes into this from the recommended entry.
constexpr std::array<unsigned, 12> kSteps = {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};
std::optional<size_t> stepIndex(unsigned percent) {
    auto found = std::find(kSteps.begin(), kSteps.end(), percent);
    if (found == kSteps.end())
        return std::nullopt;
    return size_t(found - kSteps.begin());
}
LUID adapterOf(const DisplayTarget &t) {
    LUID luid{};
    luid.LowPart = DWORD(uint64_t(t.sourceAdapterId) & 0xFFFFFFFFull);
    luid.HighPart = LONG(uint64_t(t.sourceAdapterId) >> 32);
    return luid;
}
/// The one guard that matters: we only ever address a target that is exclusively ours to scale.
const char *refuse(const DisplayTarget &t) {
    if (!t.active || !t.hasSource)
        return "the virtual display is not attached to the desktop";
    if (t.cloned)
        return "the virtual display is duplicating another monitor";
    if (t.primary)
        return "the virtual display is the primary display";
    return nullptr;
}
} // namespace
std::optional<ScaleInfo> displayScaleOf(const DisplayTarget &target) {
    if (!target.hasSource)
        return std::nullopt;
    DpiScaleGet packet{};
    packet.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(kGetDpiScaling);
    packet.header.size = sizeof packet;
    packet.header.adapterId = adapterOf(target);
    packet.header.id = target.sourceId;
    if (DisplayConfigGetDeviceInfo(&packet.header) != ERROR_SUCCESS)
        return std::nullopt;
    auto current = std::clamp(packet.currentRelative, packet.minimumRelative, packet.maximumRelative);
    // minimumRelative is at or below zero; its magnitude is how far the ladder reaches below the recommendation.
    const auto below = size_t(-std::min(packet.minimumRelative, 0));
    const auto span = below + size_t(std::max(packet.maximumRelative, 0)) + 1;
    if (span > kSteps.size() || below + size_t(current) >= kSteps.size())
        return std::nullopt;
    ScaleInfo info;
    info.recommended = kSteps[below];
    info.current = kSteps[below + size_t(current)];
    info.minimum = kSteps[0];
    info.maximum = kSteps[below + size_t(std::max(packet.maximumRelative, 0))];
    return info;
}
ScaleResult applyDisplayScale(const DisplayTarget &target, unsigned percent) {
    ScaleResult result;
    if (const char *why = refuse(target)) {
        result.problem = std::string("Display scaling was not applied because ") + why + ".";
        return result;
    }
    auto info = displayScaleOf(target);
    if (!info) {
        result.problem = "Windows did not report this display's scaling.";
        return result;
    }
    result.percent = info->current;
    const unsigned wanted = std::clamp(percent, info->minimum, info->maximum);
    if (wanted != percent)
        logInfo("Display scaling " + std::to_string(percent) + "% is outside what Windows offers for this "
                "monitor; using " + std::to_string(wanted) + "%");
    if (info->current == wanted) {
        result.applied = true;
        return result;
    }
    auto to = stepIndex(wanted), from = stepIndex(info->recommended);
    if (!to || !from) {
        result.problem = "Windows reported an unexpected scaling ladder for this display.";
        return result;
    }
    DpiScaleSet packet{};
    packet.header.type = DISPLAYCONFIG_DEVICE_INFO_TYPE(kSetDpiScaling);
    packet.header.size = sizeof packet;
    packet.header.adapterId = adapterOf(target);
    packet.header.id = target.sourceId;
    packet.relative = int32_t(*to) - int32_t(*from);
    if (DisplayConfigSetDeviceInfo(&packet.header) != ERROR_SUCCESS) {
        result.problem = "Windows refused to change this display's scaling.";
        return result;
    }
    // Trust what Windows reports back rather than what we asked for.
    if (auto after = displayScaleOf(target))
        result.percent = after->current;
    else
        result.percent = wanted;
    result.applied = result.percent == wanted;
    if (!result.applied)
        result.problem = "Windows did not keep the requested scaling.";
    return result;
}
} // namespace lm
