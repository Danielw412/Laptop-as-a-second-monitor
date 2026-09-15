#include "display_query.hpp"
#include "logging.hpp"
#include <map>
#include <tuple>
namespace bm {
namespace {
using TargetKey = std::tuple<LONG, DWORD, UINT32>;
TargetKey key(const DISPLAYCONFIG_PATH_TARGET_INFO &t) {
    return {t.adapterId.HighPart, t.adapterId.LowPart, t.id};
}
} // namespace
std::vector<DisplayTarget> queryDisplayTargets() {
    std::vector<DisplayTarget> result;
    UINT32 pathCount = 0, modeCount = 0;
    // QDC_ALL_PATHS lists inactive targets too, which is how we notice BrowserMon attached but not extended.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return result;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        auto status = QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (status != ERROR_SUCCESS)
            return result;
        paths.resize(pathCount);
        modes.resize(modeCount);
        // Active sources shared by more than one active target are clones.
        std::map<std::pair<DWORD, UINT32>, int> activeTargetsPerSource;
        for (const auto &p : paths)
            if (p.flags & DISPLAYCONFIG_PATH_ACTIVE)
                ++activeTargetsPerSource[{p.sourceInfo.adapterId.LowPart, p.sourceInfo.id}];
        std::map<TargetKey, DisplayTarget> byTarget;
        for (const auto &p : paths) {
            if (!p.targetInfo.targetAvailable)
                continue;
            const bool active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
            auto k = key(p.targetInfo);
            auto existing = byTarget.find(k);
            if (existing != byTarget.end() && (existing->second.active || !active))
                continue; // Keep the active description of a target over its inactive alternatives
            DisplayTarget t;
            t.active = active;
            t.available = true;
            DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
            name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(name), p.targetInfo.adapterId,
                           p.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
                t.devicePath = utf8(name.monitorDevicePath);
                if (name.flags.friendlyNameFromEdid)
                    t.friendlyName = utf8(name.monitorFriendlyDeviceName);
                if (name.flags.edidIdsValid) {
                    t.edidManufacturer = name.edidManufactureId;
                    t.edidProduct = name.edidProductCodeId;
                }
            }
            if (active) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), p.sourceInfo.adapterId,
                                 p.sourceInfo.id};
                if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS)
                    t.gdiName = utf8(source.viewGdiDeviceName);
                const UINT32 modeIndex = p.sourceInfo.modeInfoIdx;
                if (modeIndex != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && modeIndex < modes.size() &&
                    modes[modeIndex].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    const auto &pos = modes[modeIndex].sourceMode.position;
                    t.primary = pos.x == 0 && pos.y == 0;
                }
                t.cloned = activeTargetsPerSource[{p.sourceInfo.adapterId.LowPart, p.sourceInfo.id}] > 1;
            }
            byTarget[k] = std::move(t);
        }
        for (auto &[k, t] : byTarget)
            result.push_back(std::move(t));
        return result;
    }
    return result;
}
bool applyExtendTopology() {
    auto status = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND | SDC_PATH_PERSIST_IF_REQUIRED);
    if (status != ERROR_SUCCESS)
        logWarning("SetDisplayConfig(extend) failed with " + std::to_string(status));
    return status == ERROR_SUCCESS;
}
namespace {
DisplayMatch resolve(const std::string &gdiName, bool allowPrimary) {
    DisplayMatch match;
    for (auto &d : displays()) {
        if (d.name != gdiName)
            continue;
        if (d.primary && !allowPrimary) {
            match.problem = SelectionProblem::Primary;
            match.detail = describe(SelectionProblem::Primary);
            return match;
        }
        match.display = d;
        match.problem = SelectionProblem::None;
        return match;
    }
    match.problem = SelectionProblem::Inactive;
    match.detail = "BrowserMon is not attached to the desktop as a capturable output yet";
    return match;
}
} // namespace
DisplayMatch matchBrowserMon(bool allowPrimary) {
    auto selection = selectBrowserMon(queryDisplayTargets());
    DisplayMatch match;
    match.problem = selection.problem;
    match.detail = describe(selection.problem);
    if (!selection.display)
        return match;
    if (selection.problem == SelectionProblem::Primary && !allowPrimary)
        return match;
    if (selection.problem == SelectionProblem::Inactive || selection.problem == SelectionProblem::Cloned) {
        // Windows attached BrowserMon in Duplicate mode or left it inactive. Extend the desktop once; the caller
        // polls again after the topology change.
        static Clock::time_point lastFix{};
        if (Clock::now() - lastFix > std::chrono::seconds(10)) {
            lastFix = Clock::now();
            logInfo(std::string(describe(selection.problem)) + "; switching the desktop to Extend");
            applyExtendTopology();
        }
        return match;
    }
    return resolve(selection.display->gdiName, allowPrimary);
}
DisplayMatch matchNamedDisplay(const std::string &gdiName, bool allowPrimary) {
    auto match = resolve(gdiName, allowPrimary);
    if (!match.display && match.problem == SelectionProblem::Inactive) {
        match.problem = SelectionProblem::NotFound;
        match.detail = "Display " + gdiName + " is not connected";
    }
    return match;
}
} // namespace bm
