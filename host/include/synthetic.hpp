#pragma once
// Generated test content for controlled runs of the pipeline (bench --synthetic, --pattern). The moving bar is the
// historical source. The scrolling terminal is small, high-contrast glyphs moving every frame; motion search predicts
// pure scrolling well, so it mostly tests steady-state quality. The desktop adds what actually starves a screen
// encoder: a window switch every three seconds (the whole frame is new) and a 640x360 region whose content changes
// at 30 fps like a playing video.
#include "platform.hpp"
#include <string>
namespace lm {
enum class SyntheticContent { Bar, Scroll, Desktop };
SyntheticContent parseSyntheticContent(const std::string &);
const char *syntheticContentName(SyntheticContent);
/// Renders frame `n` of the chosen content into a BGRA texture of the given size on the caller's device. The
/// scrolling text is drawn once into an atlas with Direct2D; each frame is then one GPU copy, so the source costs
/// nothing on the CPU and is identical from run to run.
class SyntheticSource {
  public:
    SyntheticSource(ID3D11Device *, ID3D11DeviceContext *, unsigned width, unsigned height, SyntheticContent,
                    unsigned scrollPixelsPerFrame = 4);
    ~SyntheticSource();
    SyntheticSource(const SyntheticSource &) = delete;
    SyntheticSource &operator=(const SyntheticSource &) = delete;
    /// Writes frame `n` into `target` (BGRA, at least width x height, render-target bindable for the bar).
    void render(ID3D11Texture2D *target, uint64_t n);
    SyntheticContent content() const {
        return content_;
    }
    /// Vertical offset of frame `n` into the text atlas; lets an offline check rebuild the exact source frame.
    unsigned scrollOffset(uint64_t n) const;

  private:
    struct Impl;
    Impl *impl_;
    SyntheticContent content_;
};
} // namespace lm
