#pragma once
// Identifies the LaptopMon virtual monitor by what the driver's EDID says about it, never by its \\.\DISPLAYn
// number, which Windows reassigns freely. The pure selection logic here is unit-tested; the Windows queries that
// feed it live in display_query.cpp.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace lm {
/// The EDID in driver/LaptopMonitorIdd/Driver.cpp: manufacturer "LMV" (0x31B6 big-endian), product 0x0001.
inline constexpr uint16_t kLaptopMonManufacturer = 0x31B6;
inline constexpr uint16_t kLaptopMonProduct = 0x0001;
inline constexpr const char *kLaptopMonFriendlyName = "LaptopMon";
inline constexpr const char *kLaptopMonPnpId = "LMV0001";
struct DisplayTarget {
    std::string gdiName;      // \\.\DISPLAYn of the source this target is on (empty when inactive)
    std::string devicePath;   // \\?\DISPLAY#LMV0001#...#{...}
    std::string friendlyName; // From EDID descriptor, "LaptopMon"
    uint16_t edidManufacturer = 0;
    uint16_t edidProduct = 0;
    bool active = false;      // Part of the current desktop topology
    bool primary = false;     // The source at (0,0)
    bool cloned = false;      // Shares its source with another target (Duplicate mode)
    bool available = true;    // targetAvailable: the monitor is physically (virtually) attached
    // The display-config source driving this target, which is what per-display scaling is addressed to. Plain
    // integers so this header stays free of Windows types; see display_scale.cpp.
    bool hasSource = false;
    int64_t sourceAdapterId = 0; // LUID: high part in the upper 32 bits
    uint32_t sourceId = 0;
};
bool isLaptopMon(const DisplayTarget &);
enum class SelectionProblem {
    None,
    NotFound,      // No LaptopMon target at all: the virtual display is not running
    Inactive,      // Present but not part of the desktop (needs Extend)
    Cloned,        // Present but duplicating another display (needs Extend)
    Primary,       // Present but set as the primary display: refuse to stream the user's main desktop
};
struct Selection {
    std::optional<DisplayTarget> display;
    SelectionProblem problem = SelectionProblem::NotFound;
};
/// Picks LaptopMon from the current targets. Never returns another display, whatever else is connected.
Selection selectLaptopMon(const std::vector<DisplayTarget> &targets);
const char *describe(SelectionProblem);
} // namespace lm
