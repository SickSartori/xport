#pragma once

// A picture upscaled by a 64-bit helper process (tools/upscale64), for upscalers that exist only for 64-bit programs
// (NVIDIA DLSS). The port is a 32-bit process; the helper opens the textures the port shares with it by name (NT
// handles), on the same graphics card, and the two take turns on them with keyed mutexes:
//
//   port:   acquire_input (key 0) -> draws colour, depth and motion -> submit: releases them with key 1 and waits
//           for the output (key 1)
//   helper: inputs key 1, output key 0 -> upscales -> output key 1, inputs key 0
//   port:   reads the output -> release_output (key 0)
//
// What each frame needs besides the pictures (jitter, a cut) goes through a small shared memory block. The helper
// runs until the port stops it or the port's process ends, a crash too (a job object); what it writes goes to the
// port's log (set_log). Plan: sick-xport docs/UPSCALING-DLSS-FSR.md (F5).

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <thread>

namespace xport::pc {

// The names both sides give the shared objects, from the port's process ID.
std::wstring upscale_shared_name(DWORD process, const wchar_t* what);

// The shared memory block, as tools/upscale64 reads it.
struct UpscaleFrame {
    float jitter_x = 0, jitter_y = 0;  // the camera's sub-pixel offset this frame, in render pixels
    int32_t reset = 0;                 // 1: a cut, forget the previous frames
    float motion_scale_x = 1, motion_scale_y = 1;  // what the motion vectors are multiplied by to be render pixels
    uint32_t number = 0;               // counted by submit
};

class UpscaleBridge {
public:
    UpscaleBridge() = default;
    UpscaleBridge(const UpscaleBridge&) = delete;
    UpscaleBridge& operator=(const UpscaleBridge&) = delete;
    ~UpscaleBridge();

    // Makes the textures (colour BGRA8, depth R32_FLOAT and motion R16G16_FLOAT at the input size; output RGBA8) and
    // starts `helper` with `mode`: "bilinear", or "dlaa", "quality", "balanced", "performance" (DLSS). False, with
    // the reason in error(), if any of it fails or the helper does not answer within `timeout_ms` (a helper without
    // DLSS, or a card without it, ends with code 4).
    bool start(ID3D11Device* device, const std::wstring& helper, uint32_t input_width, uint32_t input_height,
               uint32_t output_width, uint32_t output_height, const char* mode = "bilinear", DWORD timeout_ms = 10000);
    void stop();

    // Where the helper's own lines go (its errors, DLSS's state): the port's log. Before start; none by default.
    void set_log(void (*log)(const char* line)) { log_ = log; }
    DWORD helper_process() const { return process_.dwProcessId; }

    ID3D11Texture2D* colour() const { return colour_shared_.texture.Get(); }
    ID3D11Texture2D* depth() const { return depth_shared_.texture.Get(); }
    ID3D11Texture2D* motion() const { return motion_shared_.texture.Get(); }
    ID3D11Texture2D* output() const { return output_shared_.texture.Get(); }
    const std::string& error() const { return error_; }

    bool acquire_input(DWORD timeout_ms = 1000);
    // The inputs are drawn: the helper takes them with `frame`; true once the output is ready (and held until
    // release_output). submit is send then receive; apart, the port can draw on while the helper works (send this
    // frame, then receive it a frame later, showing the one before).
    bool submit(ID3D11DeviceContext* context, const UpscaleFrame& frame = {}, DWORD timeout_ms = 1000);
    bool send(ID3D11DeviceContext* context, const UpscaleFrame& frame = {});
    bool receive(DWORD timeout_ms = 1000);
    void release_output();

private:
    struct Shared {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> lock;
        HANDLE handle = nullptr;
    };
    bool fail(const char* what, HRESULT hr = S_OK);
    HRESULT share(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, UINT bind, const wchar_t* what,
                  Shared& shared);

    Shared colour_shared_, depth_shared_, motion_shared_, output_shared_;
    HANDLE ready_ = nullptr, frame_mapping_ = nullptr, job_ = nullptr, log_read_ = nullptr;
    std::thread log_thread_;
    void (*log_)(const char* line) = nullptr;
    UpscaleFrame* frame_ = nullptr;
    uint32_t frames_ = 0;
    PROCESS_INFORMATION process_{};
    std::string error_;
};

}  // namespace xport::pc
