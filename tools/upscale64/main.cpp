// upscale64: the 64-bit helper of xport's upscale bridge (src/pc/upscale_bridge.h). It opens the two textures the port
// shares by name, on the same graphics card, and turns each input into an output until the port's process ends.
//
//   upscale64.exe <port process ID> <adapter LUID high:low> <input width> <input height> <output width> <output height>
//
// The upscaler is a bilinear stretch for now: the place where DLSS (NGX, 64-bit only) goes, once the port gives it
// depth, motion vectors and jitter (sick-xport docs/UPSCALING-DLSS-FSR.md).

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring shared_name(DWORD process, const wchar_t* what) {  // as upscale_shared_name in upscale_bridge.cpp
    wchar_t name[96];
    std::swprintf(name, 96, L"Local\\xport-upscale-%lu-%ls", static_cast<unsigned long>(process), what);
    return name;
}

constexpr char kShaders[] = R"(
Texture2D source : register(t0);
SamplerState linear_clamp : register(s0);
struct VSOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VSOutput vs(uint id : SV_VertexID) {
    VSOutput o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOutput i) : SV_Target { return source.SampleLevel(linear_clamp, i.uv, 0); }
)";

ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShaders, sizeof(kShaders) - 1, "upscale64", nullptr, nullptr, entry, profile, 0, 0, &code, &errors))) {
        if (errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return nullptr;
    }
    return code;
}

int fail(const char* what, HRESULT hr) {
    std::fprintf(stderr, "upscale64: %s (%08lX)\n", what, static_cast<unsigned long>(hr));
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 7) {
        std::fprintf(stderr, "usage: upscale64 <process> <luid high:low> <in w> <in h> <out w> <out h>\n");
        return 1;
    }
    const DWORD parent = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    LUID luid{};
    unsigned long high = 0, low = 0;
    if (std::swscanf(argv[2], L"%lx:%lx", &high, &low) != 2) return fail("bad adapter LUID", E_INVALIDARG);
    luid.HighPart = static_cast<LONG>(high), luid.LowPart = low;
    const UINT output_width = static_cast<UINT>(std::wcstoul(argv[5], nullptr, 10));
    const UINT output_height = static_cast<UINT>(std::wcstoul(argv[6], nullptr, 10));
    const HANDLE port = OpenProcess(SYNCHRONIZE, FALSE, parent);
    if (!port) return fail("the port's process is not there", HRESULT_FROM_WIN32(GetLastError()));

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) return fail("the port's graphics card is not found", hr);
    ComPtr<ID3D11Device> base;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &base, nullptr,
                           &context);
    ComPtr<ID3D11Device1> device;
    if (SUCCEEDED(hr)) hr = base.As(&device);
    if (FAILED(hr)) return fail("no Direct3D 11.1 device", hr);

    ComPtr<ID3D11Texture2D> input, output;
    hr = device->OpenSharedResourceByName(shared_name(parent, L"input").c_str(),
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&input));
    if (SUCCEEDED(hr)) {
        hr = device->OpenSharedResourceByName(shared_name(parent, L"output").c_str(),
                                              DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&output));
    }
    if (FAILED(hr)) return fail("cannot open the shared textures", hr);
    ComPtr<IDXGIKeyedMutex> input_lock, output_lock;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11SamplerState> sampler;
    const ComPtr<ID3DBlob> vs = compile("vs", "vs_5_0"), ps = compile("ps", "ps_5_0");
    if (!vs || !ps) return fail("shaders", E_FAIL);
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(input.As(&input_lock)) || FAILED(output.As(&output_lock)) ||
        FAILED(device->CreateShaderResourceView(input.Get(), nullptr, &source)) ||
        FAILED(device->CreateRenderTargetView(output.Get(), nullptr, &target)) ||
        FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_shader)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_shader)) ||
        FAILED(device->CreateSamplerState(&sampler_desc, &sampler))) {
        return fail("cannot set up the stretch", E_FAIL);
    }
    if (const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, shared_name(parent, L"ready").c_str())) {
        SetEvent(ready);
        CloseHandle(ready);
    }

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(output_width), static_cast<float>(output_height), 0, 1};
    for (;;) {
        // An input, or every half second a look at whether the port is still there.
        hr = input_lock->AcquireSync(1, 500);
        if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
            if (WaitForSingleObject(port, 0) == WAIT_OBJECT_0) break;
            continue;
        }
        if (FAILED(hr) || output_lock->AcquireSync(0, INFINITE) != S_OK) break;
        ID3D11RenderTargetView* rtv = target.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_shader.Get(), nullptr, 0);
        context->PSSetShader(pixel_shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = source.Get();
        context->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* s = sampler.Get();
        context->PSSetSamplers(0, 1, &s);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->Flush();
        output_lock->ReleaseSync(1);
        input_lock->ReleaseSync(0);
    }
    CloseHandle(port);
    return 0;
}
