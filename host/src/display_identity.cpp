#include "display_identity.hpp"
#include <algorithm>
#include <cctype>
namespace bm {
namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}
uint16_t swapped(uint16_t v) {
    return uint16_t((v << 8) | (v >> 8));
}
} // namespace
bool isBrowserMon(const DisplayTarget &t) {
    // The PnP hardware id in the device path is derived from the EDID manufacturer and product and is the most
    // reliable marker. DISPLAYCONFIG reports the manufacturer word in either byte order depending on the API, so
    // accept both. The friendly name alone is accepted too: it also comes straight from the EDID.
    if (upper(t.devicePath).find(std::string("DISPLAY#") + kBrowserMonPnpId + "#") != std::string::npos)
        return true;
    if ((t.edidManufacturer == kBrowserMonManufacturer || t.edidManufacturer == swapped(kBrowserMonManufacturer)) &&
        t.edidProduct == kBrowserMonProduct)
        return true;
    return t.friendlyName == kBrowserMonFriendlyName;
}
Selection selectBrowserMon(const std::vector<DisplayTarget> &targets) {
    Selection best;
    for (const auto &t : targets) {
        if (!isBrowserMon(t) || !t.available)
            continue;
        SelectionProblem problem = SelectionProblem::None;
        if (!t.active || t.gdiName.empty())
            problem = SelectionProblem::Inactive;
        else if (t.cloned)
            problem = SelectionProblem::Cloned;
        else if (t.primary)
            problem = SelectionProblem::Primary;
        // Prefer a usable instance; otherwise report the most actionable problem we saw.
        if (problem == SelectionProblem::None)
            return {t, problem};
        if (!best.display || best.problem == SelectionProblem::NotFound)
            best = {t, problem};
    }
    return best;
}
const char *describe(SelectionProblem p) {
    switch (p) {
    case SelectionProblem::None:
        return "BrowserMon found";
    case SelectionProblem::NotFound:
        return "BrowserMon is not connected";
    case SelectionProblem::Inactive:
        return "BrowserMon is attached but not part of the desktop";
    case SelectionProblem::Cloned:
        return "BrowserMon is duplicating another display";
    case SelectionProblem::Primary:
        return "BrowserMon is set as the primary display; streaming refused";
    }
    return "Unknown display state";
}
} // namespace bm
