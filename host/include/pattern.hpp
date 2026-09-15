#pragma once
#include "platform.hpp"
#include <d3d11_1.h>
namespace bm {
// Reproducible moving GPU rectangle on the explicitly selected monitor.
class Pattern {
    HWND window_{};
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11DeviceContext1> context_;
    ComPtr<ID3D11RenderTargetView> target_;
    unsigned width_,height_,frame_=0;
public:
    Pattern(Device&,const Display&);
    ~Pattern();
    void draw();
};
}
