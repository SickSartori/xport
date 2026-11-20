// The upscale bridge (upscale_bridge.h) with the 64-bit helper (tools/upscale64): a picture goes over and comes back
// twice its size, stretched; then, when the helper has DLSS and the card runs it, through DLSS. Skipped (77) when
// the helper is not built: set <GAME>_UPSCALE64 to its path, or put upscale64.exe next to this test.

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
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

// One pixel of a texture (as stored: BGRA or RGBA), through a copy the CPU can read.
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

void fill(ID3D11DeviceContext* context, ID3D11Texture2D* texture, uint32_t value, UINT width, UINT height) {
    const std::vector<uint32_t> data(size_t{width} * height, value);
    context->UpdateSubresource(texture, 0, nullptr, data.data(), width * 4, 0);
}

float halton(int index, int base) {
    float f = 1, r = 0;
    for (int i = index; i > 0; i /= base) f /= base, r += f * (i % base);
    return r;
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

    // What the helper writes reaches the port's log: an unknown mode makes it say so and end (code 4).
    {
        static std::mutex lines_mutex;
        static std::vector<std::string> lines;
        UpscaleBridge bridge;
        bridge.set_log([](const char* line) {
            std::lock_guard<std::mutex> lock(lines_mutex);
            lines.emplace_back(line);
        });
        CHECK(!bridge.start(device.Get(), helper, 64, 32, 128, 64, "no-such-mode"));
        std::printf("unknown mode: %s\n", bridge.error().c_str());
        std::lock_guard<std::mutex> lock(lines_mutex);
        CHECK(!lines.empty() && lines[0].rfind("upscale64: ", 0) == 0);
        for (const std::string& line : lines) std::printf("  helper said: %s\n", line.c_str());
    }

    // Stretched: left red and right blue, then the other way round; each comes back twice as large (RGBA).
    {
        UpscaleBridge bridge;
        const bool started = bridge.start(device.Get(), helper, 64, 32, 128, 64);
        if (!started) std::printf("start: %s\n", bridge.error().c_str());
        CHECK(started);
        for (int frame = 0; started && frame < 2; ++frame) {
            const uint32_t left = frame == 0 ? 0xFFFF0000u : 0xFF0000FFu, right = frame == 0 ? 0xFF0000FFu : 0xFFFF0000u;  // BGRA
            std::vector<uint32_t> pixels(64 * 32);
            for (UINT y = 0; y < 32; ++y) {
                for (UINT x = 0; x < 64; ++x) pixels[y * 64 + x] = x < 32 ? left : right;
            }
            CHECK(bridge.acquire_input());
            context->UpdateSubresource(bridge.colour(), 0, nullptr, pixels.data(), 64 * 4, 0);
            const bool done = bridge.submit(context.Get(), {}, 3000);
            CHECK(done);
            if (!done) break;
            const auto rgba = [](uint32_t bgra) { return (bgra & 0xFF00FF00u) | ((bgra >> 16) & 0xFF) | ((bgra & 0xFF) << 16); };
            CHECK(pixel(device.Get(), context.Get(), bridge.output(), 10, 32) == rgba(left));
            CHECK(pixel(device.Get(), context.Get(), bridge.output(), 117, 32) == rgba(right));
            bridge.release_output();
        }
    }

    // Through DLSS (Quality, 1280x720 to 1920x1080): a still red picture with the jitter a game would use comes out
    // red at the output's size; and the round trip's time, printed.
    UpscaleBridge dlss;
    if (!dlss.start(device.Get(), helper, 1280, 720, 1920, 1080, "quality")) {
        std::printf("DLSS skipped: %s\n", dlss.error().c_str());
        return test_result();
    }
    D3D11_TEXTURE2D_DESC output_desc{};
    dlss.output()->GetDesc(&output_desc);
    CHECK(output_desc.Width == 1920 && output_desc.Height == 1080);
    LARGE_INTEGER frequency{}, begin{}, end{};
    QueryPerformanceFrequency(&frequency);
    constexpr int kFrames = 120;
    int done = 0;
    uint32_t centre = 0;
    for (int i = 0; i < kFrames; ++i) {
        if (i == 20) QueryPerformanceCounter(&begin);  // after the warm-up
        if (!dlss.acquire_input()) break;
        if (i == 0) {
            fill(context.Get(), dlss.colour(), 0xFFFF0000u, 1280, 720);  // BGRA red
            fill(context.Get(), dlss.depth(), 0x3F000000u, 1280, 720);   // 0.5f
            fill(context.Get(), dlss.motion(), 0, 1280, 720);            // no motion
        }
        UpscaleFrame frame;
        frame.jitter_x = halton(i % 16 + 1, 2) - 0.5f;
        frame.jitter_y = halton(i % 16 + 1, 3) - 0.5f;
        frame.reset = i == 0;
        if (!dlss.submit(context.Get(), frame, 3000)) break;
        if (i == kFrames - 1) centre = pixel(device.Get(), context.Get(), dlss.output(), 960, 540);
        dlss.release_output();
        ++done;
    }
    QueryPerformanceCounter(&end);
    CHECK(done == kFrames);
    const uint32_t r = centre & 0xFF, g = (centre >> 8) & 0xFF, b = (centre >> 16) & 0xFF;  // RGBA
    std::printf("DLSS centre pixel: %u %u %u\n", r, g, b);
    CHECK(r > 200 && g < 40 && b < 40);
    std::printf("DLSS Quality 1280x720 -> 1920x1080 through the bridge: %.2f ms a frame\n",
                (end.QuadPart - begin.QuadPart) * 1000.0 / frequency.QuadPart / std::max(done - 20, 1));
    dlss.stop();

    // Which way the motion vectors point. Fine stripes (a sine of 7 render pixels) scroll right, 3 render pixels a
    // frame; DLSS gets the vectors as previous minus current position (what camera_motion.h writes), as zero, and
    // turned the other way. With the right way its history lines up with the picture and the stripes stay sharp and
    // in place; otherwise it blends stripes of other phases. The error is against the stripes worked out exactly at
    // the output's pixels, along the middle row.
    const auto stripes_error = [&](float sign) -> double {
        constexpr UINT kWidth = 1280, kHeight = 720, kOutWidth = 1920, kOutHeight = 1080;
        constexpr double kPeriod = 7, kStep = 3, kPi = 3.14159265358979;
        constexpr int kSteps = 40;
        const auto stripes = [&](double u, int frame) { return 0.5 + 0.5 * std::sin(2 * kPi * (u - kStep * frame) / kPeriod); };
        UpscaleBridge scroll;
        if (!scroll.start(device.Get(), helper, kWidth, kHeight, kOutWidth, kOutHeight, "quality")) return -1;
        std::vector<uint32_t> colour(size_t{kWidth} * kHeight);
        // R16G16_FLOAT: x = -3 * sign (half floats: -3 is 0xC200, 3 is 0x4200), y = 0.
        const std::vector<uint32_t> motion(size_t{kWidth} * kHeight, sign == 0 ? 0u : sign > 0 ? 0xC200u : 0x4200u);
        double error = 0;
        for (int i = 0; i < kSteps; ++i) {
            for (UINT x = 0; x < kWidth; ++x) {
                const auto v = static_cast<uint32_t>(std::lround(stripes(x + 0.5, i) * 255));
                colour[x] = 0xFF000000u | (v << 16) | (v << 8) | v;
            }
            for (UINT y = 1; y < kHeight; ++y) std::copy_n(colour.begin(), kWidth, colour.begin() + size_t{y} * kWidth);
            if (!scroll.acquire_input()) return -1;
            context->UpdateSubresource(scroll.colour(), 0, nullptr, colour.data(), kWidth * 4, 0);
            if (i == 0) {
                context->UpdateSubresource(scroll.motion(), 0, nullptr, motion.data(), kWidth * 4, 0);
                fill(context.Get(), scroll.depth(), 0x3F000000u, kWidth, kHeight);
            }
            UpscaleFrame frame;
            frame.reset = i == 0;
            if (!scroll.submit(context.Get(), frame, 3000)) return -1;
            if (i == kSteps - 1) {
                D3D11_TEXTURE2D_DESC desc{};
                scroll.output()->GetDesc(&desc);
                desc.BindFlags = 0, desc.MiscFlags = 0, desc.Usage = D3D11_USAGE_STAGING, desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return -1;
                context->CopyResource(staging.Get(), scroll.output());
                if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return -1;
                const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * (kOutHeight / 2));
                for (UINT x = 32; x < kOutWidth - 32; ++x) {  // away from the sides, where the history runs out
                    const double truth = stripes((x + 0.5) * kWidth / kOutWidth, i) * 255;
                    error += std::abs(double(row[x] & 0xFF) - truth) / (kOutWidth - 64);
                }
                context->Unmap(staging.Get(), 0);
            }
            scroll.release_output();
        }
        return error;
    };
    const double right_way = stripes_error(1), no_motion = stripes_error(0), wrong_way = stripes_error(-1);
    std::printf("scrolling stripes, mean error against the truth (of 255): vectors previous-minus-current %.1f, none %.1f,"
                " reversed %.1f\n", right_way, no_motion, wrong_way);
    CHECK(right_way >= 0 && no_motion >= 0 && wrong_way >= 0);
    CHECK(right_way < wrong_way);
    CHECK(right_way < no_motion);
    return test_result();
}
