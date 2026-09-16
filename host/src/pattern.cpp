#include "pattern.hpp"
namespace lm {
Pattern::Pattern(Device& device,const Display& display):width_(display.rect.right-display.rect.left),height_(display.rect.bottom-display.rect.top){
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"LaptopMonitorBenchmark";RegisterClassW(&wc);
    window_=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Laptop Monitor benchmark",WS_POPUP|WS_VISIBLE,display.rect.left,display.rect.top,width_,height_,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window_)throw std::runtime_error("Create benchmark window failed");
    ComPtr<IDXGIFactory2> factory;check(display.adapter->GetParent(IID_PPV_ARGS(&factory)),"Pattern DXGI factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width_;desc.Height=height_;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    check(factory->CreateSwapChainForHwnd(device.device.Get(),window_,&desc,nullptr,nullptr,&swapchain_),"Pattern swapchain");
    ComPtr<ID3D11Texture2D> backbuffer;check(swapchain_->GetBuffer(0,IID_PPV_ARGS(&backbuffer)),"Pattern backbuffer");check(device.device->CreateRenderTargetView(backbuffer.Get(),nullptr,&target_),"Pattern render target");check(device.context.As(&context_),"Pattern context");
}
Pattern::~Pattern(){if(window_)DestroyWindow(window_);}
void Pattern::draw(){float background[]{.04f,.08f,.12f,1};float foreground[]{.2f,.8f,.5f,1};context_->ClearRenderTargetView(target_.Get(),background);LONG x=(frame_++*13)%width_;D3D11_RECT rect{x,LONG(height_/4),std::min<LONG>(x+240,width_),LONG(height_*3/4)};context_->ClearView(target_.Get(),foreground,&rect,1);check(swapchain_->Present(0,0),"Pattern present");}
}
