// The upscale bridge (upscale_bridge.h) with the 64-bit helper (tools/upscale64): a picture goes over and comes back
// twice its size. Skipped (77) when the helper is not built: set <GAME>_UPSCALE64 to its path, or put upscale64.exe
// next to this test.

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "pc/port.h"
#include "pc/upscale_bridge.h"
#include "test.h"

using Microsoft::WRL::ComPtr;
using namespace xport::pc;

namespace {

std::wstring helper_path() {
    wchar_t value[MAX_PATH] = {};
    if (GetEnvironmentVariableW(XPORT_ENV_W("UPSCALE64"), value, MAX_PATH)) return value;
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return (std::filesystem::path(self).parent_path() / L"upscale64.exe").wstring();
}

// BGRA of one pixel of a texture, through a copy the CPU can read.
uint32_t pixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT x, UINT y) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return 0;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 0;
    const uint32_t value = static_cast<const uint32_t*>(mapped.pData)[y * (mapped.RowPitch / 4) + x];
    context->Unmap(staging.Get(), 0);
    return value;
}

}  // namespace

int main() {
    const std::wstring helper = helper_path();
    if (!std::filesystem::exists(helper)) {
        std::printf("skipped: no upscale64.exe (%ls)\n", helper.c_str());
        return 77;
    }
    // The device on the card the renderer picks (the high-performance one).
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::printf("skipped: no Direct3D 11 device\n");
        return 77;
    }

    UpscaleBridge bridge;
    const bool started = bridge.start(device.Get(), helper, 64, 32, 128, 64);
    if (!started) std::printf("start: %s\n", bridge.error().c_str());
    CHECK(started);
    if (!started) return test_result();

    // Two frames: left red and right blue, then the other way round. Each comes back twice as large.
    for (int frame = 0; frame < 2; ++frame) {
        const uint32_t left = frame == 0 ? 0xFFFF0000u : 0xFF0000FFu, right = frame == 0 ? 0xFF0000FFu : 0xFFFF0000u;
        std::vector<uint32_t> pixels(64 * 32);
        for (UINT y = 0; y < 32; ++y) {
            for (UINT x = 0; x < 64; ++x) pixels[y * 64 + x] = x < 32 ? left : right;
        }
        CHECK(bridge.acquire_input());
        context->UpdateSubresource(bridge.input(), 0, nullptr, pixels.data(), 64 * 4, 0);
        const bool done = bridge.submit(context.Get(), 3000);
        CHECK(done);
        if (!done) break;
        CHECK(pixel(device.Get(), context.Get(), bridge.output(), 10, 32) == left);
        CHECK(pixel(device.Get(), context.Get(), bridge.output(), 117, 32) == right);
        bridge.release_output();
    }
    bridge.stop();

    // The cost of the round trip at a game's sizes (720p to 1440p), for the log: printed, not checked.
    UpscaleBridge large;
    if (large.start(device.Get(), helper, 1280, 720, 2560, 1440)) {
        LARGE_INTEGER frequency{}, begin{}, end{};
        QueryPerformanceFrequency(&frequency);
        constexpr int kFrames = 200;
        int done = 0;
        QueryPerformanceCounter(&begin);
        for (int i = 0; i < kFrames; ++i) {
            if (!large.acquire_input()) break;
            const float colour[4] = {i / float(kFrames), 0.5f, 0.25f, 1};
            ComPtr<ID3D11RenderTargetView> rtv;
            device->CreateRenderTargetView(large.input(), nullptr, &rtv);
            context->ClearRenderTargetView(rtv.Get(), colour);
            if (!large.submit(context.Get(), 3000)) break;
            large.release_output();
            ++done;
        }
        QueryPerformanceCounter(&end);
        CHECK(done == kFrames);
        std::printf("round trip 1280x720 -> 2560x1440: %.3f ms a frame over %d frames\n",
                    (end.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart / std::max(done, 1), done);
    }
    return test_result();
}
