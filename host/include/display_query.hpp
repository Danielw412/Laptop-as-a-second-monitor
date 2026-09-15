#pragma once
// Windows side of BrowserMon detection: reads the display topology, fixes Duplicate/inactive attachment, and maps
// the selected target to the DXGI output the capture pipeline needs.
#include "display_identity.hpp"
#include "platform.hpp"
#include <optional>
#include <string>
namespace bm {
/// Every monitor target Windows knows about right now (active and inactive), with EDID-derived identity.
std::vector<DisplayTarget> queryDisplayTargets();
/// Switches the desktop to Extend mode (what Win+P "Extend" does). Used when BrowserMon is cloned or inactive.
bool applyExtendTopology();
struct DisplayMatch {
    std::optional<Display> display; // DXGI output for capture
    SelectionProblem problem = SelectionProblem::NotFound;
    std::string detail;             // User-facing explanation when display is empty
};
/// Finds BrowserMon by identity and resolves it to its DXGI output. Never returns any other display.
DisplayMatch matchBrowserMon(bool allowPrimary = false);
/// Finds an explicitly named output (bench only). Refuses primary displays unless allowed.
DisplayMatch matchNamedDisplay(const std::string &gdiName, bool allowPrimary);
} // namespace bm
