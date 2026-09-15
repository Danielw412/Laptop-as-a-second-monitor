#pragma once
// Identifies the BrowserMon virtual monitor by what the driver's EDID says about it, never by its \\.\DISPLAYn
// number, which Windows reassigns freely. The pure selection logic here is unit-tested; the Windows queries that
// feed it live in display_query.cpp.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace bm {
/// The EDID in driver/BrowserMonitorIdd/Driver.cpp: manufacturer "BMV" (0x09B6 big-endian), product 0x0001.
inline constexpr uint16_t kBrowserMonManufacturer = 0x09B6;
inline constexpr uint16_t kBrowserMonProduct = 0x0001;
inline constexpr const char *kBrowserMonFriendlyName = "BrowserMon";
inline constexpr const char *kBrowserMonPnpId = "BMV0001";
struct DisplayTarget {
    std::string gdiName;      // \\.\DISPLAYn of the source this target is on (empty when inactive)
    std::string devicePath;   // \\?\DISPLAY#BMV0001#...#{...}
    std::string friendlyName; // From EDID descriptor, "BrowserMon"
    uint16_t edidManufacturer = 0;
    uint16_t edidProduct = 0;
    bool active = false;      // Part of the current desktop topology
    bool primary = false;     // The source at (0,0)
    bool cloned = false;      // Shares its source with another target (Duplicate mode)
    bool available = true;    // targetAvailable: the monitor is physically (virtually) attached
};
bool isBrowserMon(const DisplayTarget &);
enum class SelectionProblem {
    None,
    NotFound,      // No BrowserMon target at all: the virtual display is not running
    Inactive,      // Present but not part of the desktop (needs Extend)
    Cloned,        // Present but duplicating another display (needs Extend)
    Primary,       // Present but set as the primary display: refuse to stream the user's main desktop
};
struct Selection {
    std::optional<DisplayTarget> display;
    SelectionProblem problem = SelectionProblem::NotFound;
};
/// Picks BrowserMon from the current targets. Never returns another display, whatever else is connected.
Selection selectBrowserMon(const std::vector<DisplayTarget> &targets);
const char *describe(SelectionProblem);
} // namespace bm
