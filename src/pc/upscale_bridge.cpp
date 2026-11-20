#include "pc/upscale_bridge.h"

#include <dxgi1_2.h>

#include <cstdio>

namespace xport::pc {

using Microsoft::WRL::ComPtr;

std::wstring upscale_shared_name(DWORD process, const wchar_t* what) {
    wchar_t name[96];
    std::swprintf(name, 96, L"Local\\xport-upscale-%lu-%ls", static_cast<unsigned long>(process), what);
    return name;
}

UpscaleBridge::~UpscaleBridge() { stop(); }

bool UpscaleBridge::fail(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof(text), hr == S_OK ? "%s" : "%s (%08lX)", what, static_cast<unsigned long>(hr));
    error_ = text;
    stop();
    return false;
}

bool UpscaleBridge::start(ID3D11Device* device, const std::wstring& helper, uint32_t input_width, uint32_t input_height,
                          uint32_t output_width, uint32_t output_height, DWORD timeout_ms) {
    stop();
    error_.clear();
    const DWORD self = GetCurrentProcessId();
    const auto make = [&](uint32_t width, uint32_t height, const wchar_t* what, ComPtr<ID3D11Texture2D>& texture,
                          ComPtr<IDXGIKeyedMutex>& lock, HANDLE& handle) -> HRESULT {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
        ComPtr<IDXGIResource1> shared;
        if (SUCCEEDED(hr)) hr = texture.As(&shared);
        if (SUCCEEDED(hr)) {
            hr = shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                            upscale_shared_name(self, what).c_str(), &handle);
        }
        if (SUCCEEDED(hr)) hr = texture.As(&lock);
        return hr;
    };
    if (const HRESULT hr = make(input_width, input_height, L"input", input_, input_lock_, input_handle_); FAILED(hr)) {
        return fail("cannot share the input texture", hr);
    }
    if (const HRESULT hr = make(output_width, output_height, L"output", output_, output_lock_, output_handle_); FAILED(hr)) {
        return fail("cannot share the output texture", hr);
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapter_desc))) {
        return fail("cannot tell the graphics card");
    }
    ready_ = CreateEventW(nullptr, TRUE, FALSE, upscale_shared_name(self, L"ready").c_str());
    if (!ready_) return fail("cannot make the ready event", HRESULT_FROM_WIN32(GetLastError()));
    // The helper: the port's process (its names, and it ends with it), the card (LUID), the two sizes.
    wchar_t arguments[512];
    std::swprintf(arguments, 512, L"\"%ls\" %lu %08lX:%08lX %u %u %u %u", helper.c_str(), static_cast<unsigned long>(self),
                  static_cast<unsigned long>(adapter_desc.AdapterLuid.HighPart),
                  static_cast<unsigned long>(adapter_desc.AdapterLuid.LowPart), input_width, input_height, output_width,
                  output_height);
    STARTUPINFOW startup{sizeof(startup)};
    if (!CreateProcessW(helper.c_str(), arguments, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process_)) {
        return fail("cannot start the helper", HRESULT_FROM_WIN32(GetLastError()));
    }
    const HANDLE waits[2] = {ready_, process_.hProcess};
    const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
    if (woke != WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process_.hProcess, &code);
        char text[96];
        std::snprintf(text, sizeof(text), woke == WAIT_OBJECT_0 + 1 ? "the helper ended (code %lu)" : "the helper did not answer",
                      static_cast<unsigned long>(code));
        return fail(text);
    }
    return true;
}

void UpscaleBridge::stop() {
    if (process_.hProcess) {
        if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(process_.hProcess, 0);
        CloseHandle(process_.hProcess);
        CloseHandle(process_.hThread);
        process_ = {};
    }
    for (HANDLE* handle : {&input_handle_, &output_handle_, &ready_}) {
        if (*handle) CloseHandle(*handle);
        *handle = nullptr;
    }
    input_lock_.Reset();
    output_lock_.Reset();
    input_.Reset();
    output_.Reset();
}

bool UpscaleBridge::acquire_input(DWORD timeout_ms) {
    return input_lock_ && input_lock_->AcquireSync(0, timeout_ms) == S_OK;
}

bool UpscaleBridge::submit(ID3D11DeviceContext* context, DWORD timeout_ms) {
    if (!input_lock_ || !output_lock_) return false;
    context->Flush();
    if (FAILED(input_lock_->ReleaseSync(1))) return false;
    return output_lock_->AcquireSync(1, timeout_ms) == S_OK;
}

void UpscaleBridge::release_output() {
    if (output_lock_) output_lock_->ReleaseSync(0);
}

}  // namespace xport::pc
