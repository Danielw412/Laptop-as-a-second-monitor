#pragma once
// Per-display Windows scaling for the virtual monitor, and nothing else.
//
// The driver's EDID already describes a 13.3" panel, so Windows *recommends* about 150% for this monitor on a
// fresh connection. That is the whole mechanism when the user leaves the setting on Recommended. An explicit
// choice is applied through the same per-source display-config channel that the scaling slider in
// Settings > System > Display writes to. It is addressed by the source path of one display, so the host's real
// monitors and the global scaling are never touched.
#include "display_identity.hpp"
#include <optional>
#include <string>
namespace lm {
enum class DisplayScale { Recommended, Percent100, Percent125, Percent150, Percent175 };
/// The percentage a preset asks for, or 0 for Recommended (keep whatever Windows derived from the EDID).
inline unsigned scalePercent(DisplayScale s) {
    switch (s) {
    case DisplayScale::Percent100:
        return 100;
    case DisplayScale::Percent125:
        return 125;
    case DisplayScale::Percent150:
        return 150;
    case DisplayScale::Percent175:
        return 175;
    default:
        return 0;
    }
}
inline const char *scaleName(DisplayScale s) {
    switch (s) {
    case DisplayScale::Percent100:
        return "100";
    case DisplayScale::Percent125:
        return "125";
    case DisplayScale::Percent150:
        return "150";
    case DisplayScale::Percent175:
        return "175";
    default:
        return "recommended";
    }
}
inline DisplayScale scaleFromName(const std::string &v) {
    if (v == "100")
        return DisplayScale::Percent100;
    if (v == "125")
        return DisplayScale::Percent125;
    if (v == "150")
        return DisplayScale::Percent150;
    if (v == "175")
        return DisplayScale::Percent175;
    return DisplayScale::Recommended;
}
#ifdef _WIN32
struct ScaleInfo {
    unsigned current = 0;     // What this display is scaled by now
    unsigned recommended = 0; // What Windows derives from the EDID
    unsigned minimum = 0, maximum = 0;
};
/// Reads the scaling of one active display source. Empty when Windows will not report it.
std::optional<ScaleInfo> displayScaleOf(const DisplayTarget &);
struct ScaleResult {
    bool applied = false; // The display is now at the requested percentage
    unsigned percent = 0; // What it ended up at, as far as we could tell
    std::string problem;  // User-facing reason when `applied` is false
};
/// Scales one display to `percent`. Refuses anything that is not an active, non-primary, non-cloned target, so a
/// mistaken call can never resize the user's own desktop.
ScaleResult applyDisplayScale(const DisplayTarget &, unsigned percent);
#endif
} // namespace lm
