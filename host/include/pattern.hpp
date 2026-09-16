#pragma once
#include "platform.hpp"
#include <atomic>
#include <d3d11_1.h>
#include <thread>
namespace bm {
// Reproducible moving GPU rectangle on the explicitly selected monitor. It runs on its own thread with its own
// D3D11 device and presents with vsync, so it is a true one-new-frame-per-refresh source that does not share the
// pipeline's device context or pace itself off the capture loop.
class Pattern {
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> presented_{0};
    void run(const Display &, HANDLE ready, std::string &error);

  public:
    explicit Pattern(const Display &);
    ~Pattern();
    /// Frames presented so far.
    uint64_t presented() const {
        return presented_.load();
    }
};
} // namespace bm
