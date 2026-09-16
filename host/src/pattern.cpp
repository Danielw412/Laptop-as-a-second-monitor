#include "pattern.hpp"
namespace lm {
Pattern::Pattern(const Display &display) {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready)
        throw std::runtime_error("Pattern event");
    std::string error;
    thread_ = std::thread([this, &display, ready, &error] { run(display, ready, error); });
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    if (!error.empty()) {
        thread_.join();
        throw std::runtime_error(error);
    }
}
Pattern::~Pattern() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}
void Pattern::run(const Display &display, HANDLE ready, std::string &error) {
    const unsigned width = display.rect.right - display.rect.left, height = display.rect.bottom - display.rect.top;
    HWND window = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11RenderTargetView> target;
    try {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LaptopMonitorBenchmark";
        RegisterClassW(&wc);
        window = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Laptop Monitor benchmark",
                                 WS_POPUP | WS_VISIBLE, display.rect.left, display.rect.top, width, height, nullptr,
                                 nullptr, wc.hInstance, nullptr);
        if (!window)
            throw std::runtime_error("Create benchmark window failed");
        ComPtr<ID3D11DeviceContext> immediate;
        check(D3D11CreateDevice(display.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                &immediate),
              "Pattern D3D11 device");
        check(immediate.As(&context), "Pattern context");
        ComPtr<IDXGIFactory2> factory;
        check(display.adapter->GetParent(IID_PPV_ARGS(&factory)), "Pattern DXGI factory");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        check(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swapchain),
              "Pattern swapchain");
        ComPtr<ID3D11Texture2D> backbuffer;
        check(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "Pattern backbuffer");
        check(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target), "Pattern render target");
    } catch (const std::exception &e) {
        error = e.what();
        if (window)
            DestroyWindow(window);
        SetEvent(ready);
        return;
    }
    SetEvent(ready);
    unsigned frame = 0;
    while (!stop_) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        float background[]{.04f, .08f, .12f, 1};
        float foreground[]{.2f, .8f, .5f, 1};
        context->ClearRenderTargetView(target.Get(), background);
        LONG x = (frame++ * 13) % width;
        D3D11_RECT rect{x, LONG(height / 4), std::min<LONG>(x + 240, width), LONG(height * 3 / 4)};
        context->ClearView(target.Get(), foreground, &rect, 1);
        // Vsync: exactly one new desktop frame per refresh of the selected display.
        if (FAILED(swapchain->Present(1, 0)))
            break;
        ++presented_;
    }
    DestroyWindow(window);
}
} // namespace lm
