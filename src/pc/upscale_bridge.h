#pragma once

// A picture upscaled by a 64-bit helper process (tools/upscale64), for upscalers that exist only for 64-bit programs
// (NVIDIA DLSS). The port is a 32-bit process; the helper opens two textures the port shares with it by name (NT
// handles), on the same graphics card, and the two take turns on them with keyed mutexes:
//
//   port:   acquire_input (key 0) -> draws the input -> submit: releases it with key 1, waits for the output (key 1)
//   helper: input key 1, output key 0 -> upscales -> output key 1, input key 0
//   port:   reads the output -> release_output (key 0)
//
// The helper runs until the port's process ends. Plan: sick-xport docs/UPSCALING-DLSS-FSR.md (F5).

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace xport::pc {

// The names both sides give the shared objects, from the port's process ID.
std::wstring upscale_shared_name(DWORD process, const wchar_t* what);

class UpscaleBridge {
public:
    UpscaleBridge() = default;
    UpscaleBridge(const UpscaleBridge&) = delete;
    UpscaleBridge& operator=(const UpscaleBridge&) = delete;
    ~UpscaleBridge();

    // Makes the two textures (BGRA8) and starts `helper`; false (and the reason in error()) if any of it fails or the
    // helper does not answer within `timeout_ms`.
    bool start(ID3D11Device* device, const std::wstring& helper, uint32_t input_width, uint32_t input_height,
               uint32_t output_width, uint32_t output_height, DWORD timeout_ms = 5000);
    void stop();

    ID3D11Texture2D* input() const { return input_.Get(); }
    ID3D11Texture2D* output() const { return output_.Get(); }
    const std::string& error() const { return error_; }

    bool acquire_input(DWORD timeout_ms = 1000);
    // The input is drawn: the helper takes it; true once the output is ready (and held until release_output).
    bool submit(ID3D11DeviceContext* context, DWORD timeout_ms = 1000);
    void release_output();

private:
    bool fail(const char* what, HRESULT hr = S_OK);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> input_, output_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> input_lock_, output_lock_;
    HANDLE input_handle_ = nullptr, output_handle_ = nullptr, ready_ = nullptr;
    PROCESS_INFORMATION process_{};
    std::string error_;
};

}  // namespace xport::pc
