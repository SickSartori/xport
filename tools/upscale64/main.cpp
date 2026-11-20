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
#include <dxgi1_6.h>
#include <wrl/client.h>

#ifdef XPORT_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

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

#ifdef XPORT_DLSS
// DLSS made and run on made-up frames (a gradient, flat depth, no motion, the jitter a game would use): the time a
// frame takes on the GPU, from timestamp queries, for `mode` from width x height to 2560x1440.
void bench_dlss(ID3D11Device* device, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_PerfQuality_Value mode, const char* name,
                unsigned width, unsigned height) {
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    const auto texture = [&](unsigned w, unsigned h, DXGI_FORMAT format, UINT bind, const void* data, UINT pitch) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w, desc.Height = h, desc.MipLevels = 1, desc.ArraySize = 1, desc.Format = format;
        desc.SampleDesc.Count = 1, desc.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA initial{data, pitch, 0};
        ComPtr<ID3D11Texture2D> t;
        device->CreateTexture2D(&desc, data ? &initial : nullptr, &t);
        return t;
    };
    std::vector<uint32_t> pixels(size_t{width} * height);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) pixels[size_t{y} * width + x] = 0xFF000000u | ((x * 255 / width) << 16) | ((y * 255 / height) << 8) | ((x ^ y) & 0xFF);
    }
    const std::vector<float> depths(size_t{width} * height, 0.5f);
    const std::vector<uint32_t> no_motion(size_t{width} * height, 0);  // R16G16_FLOAT zeros
    const ComPtr<ID3D11Texture2D> colour = texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, pixels.data(), width * 4);
    const ComPtr<ID3D11Texture2D> depth = texture(width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depths.data(), width * 4);
    const ComPtr<ID3D11Texture2D> motion = texture(width, height, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, no_motion.data(), width * 4);
    const ComPtr<ID3D11Texture2D> output = texture(2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    if (!colour || !depth || !motion || !output) {
        std::printf("  %-11s textures could not be made\n", name);
        return;
    }
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = width, create.Feature.InHeight = height;
    create.Feature.InTargetWidth = 2560, create.Feature.InTargetHeight = 1440;
    create.Feature.InPerfQualityValue = mode;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;  // vectors at the render size, LDR colour
    NVSDK_NGX_Handle* dlss = nullptr;
    if (const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(context.Get(), &dlss, parameters, &create); NVSDK_NGX_FAILED(r)) {
        std::printf("  %-11s DLSS could not be made: %08X\n", name, static_cast<unsigned>(r));
        return;
    }
    D3D11_QUERY_DESC query_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint, start, stop;
    device->CreateQuery(&query_desc, &disjoint);
    query_desc.Query = D3D11_QUERY_TIMESTAMP;
    device->CreateQuery(&query_desc, &start);
    device->CreateQuery(&query_desc, &stop);
    double total_ms = 0;
    int timed = 0, failed = 0;
    constexpr int kFrames = 60;
    for (int frame = 0; frame < kFrames; ++frame) {
        NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
        eval.Feature.pInColor = colour.Get();
        eval.Feature.pInOutput = output.Get();
        eval.pInDepth = depth.Get();
        eval.pInMotionVectors = motion.Get();
        // Halton (2, 3), centred on the pixel: the sequence a game's jitter would follow.
        const auto halton = [](int index, int base) {
            float f = 1, r = 0;
            for (int i = index; i > 0; i /= base) f /= base, r += f * (i % base);
            return r;
        };
        eval.InJitterOffsetX = halton(frame % 16 + 1, 2) - 0.5f;
        eval.InJitterOffsetY = halton(frame % 16 + 1, 3) - 0.5f;
        eval.InRenderSubrectDimensions = {width, height};
        eval.InReset = frame == 0;
        eval.InMVScaleX = eval.InMVScaleY = 1;
        context->Begin(disjoint.Get());
        context->End(start.Get());
        if (NVSDK_NGX_FAILED(NGX_D3D11_EVALUATE_DLSS_EXT(context.Get(), dlss, parameters, &eval))) ++failed;
        context->End(stop.Get());
        context->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
        UINT64 t0 = 0, t1 = 0;
        while (context->GetData(disjoint.Get(), &clock, sizeof(clock), 0) == S_FALSE) Sleep(0);
        context->GetData(start.Get(), &t0, sizeof(t0), 0);
        context->GetData(stop.Get(), &t1, sizeof(t1), 0);
        if (frame >= 10 && !clock.Disjoint && clock.Frequency) {  // the first frames warm up
            total_ms += (t1 - t0) * 1000.0 / clock.Frequency;
            ++timed;
        }
    }
    std::printf("  %-11s %ux%u -> 2560x1440: %.2f ms a frame on the GPU (%d frames, %d failed)\n", name, width, height,
                timed ? total_ms / timed : 0.0, timed, failed);
    NVSDK_NGX_D3D11_ReleaseFeature(dlss);
}

// upscale64 --probe: whether DLSS Super Resolution runs on the high-performance graphics card (NGX on Direct3D 11),
// and the render sizes it asks for a 2560x1440 output. 0: available, 3: not available, 2: NGX did not start.
int probe_dlss() {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    DXGI_ADAPTER_DESC1 adapter_desc{};
    if (adapter) adapter->GetDesc1(&adapter_desc);
    std::printf("card: %ls\n", adapter_desc.Description);
    ComPtr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                       &device, nullptr, nullptr);
        FAILED(hr)) {
        return fail("no Direct3D 11 device", hr);
    }
    // NGX looks for nvngx_dlss.dll next to this program and writes its logs in %TEMP%\xport-upscale64.
    wchar_t self[MAX_PATH] = {}, temp[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring folder = self;
    folder.resize(folder.find_last_of(L'\\'));
    GetTempPathW(MAX_PATH, temp);
    const std::wstring logs = std::wstring(temp) + L"xport-upscale64";
    CreateDirectoryW(logs.c_str(), nullptr);
    const wchar_t* paths[] = {folder.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    // A project of a custom engine: xport's own ID (any fixed GUID; NVIDIA's application IDs are for registered titles).
    NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID("6f0c2b9e-31d4-4a55-9b6e-58d3c0a1e7f2", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                  "xport", logs.c_str(), device.Get(), &info);
    if (NVSDK_NGX_FAILED(result)) {
        std::printf("NGX did not start: %08X\n", static_cast<unsigned>(result));
        return 2;
    }
    NVSDK_NGX_Parameter* parameters = nullptr;
    int available = 0, needs_driver = 0;
    unsigned int driver_major = 0, driver_minor = 0;
    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters);
    if (NVSDK_NGX_SUCCEED(result)) {
        NVSDK_NGX_Parameter_GetI(parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
        NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &driver_major);
        NVSDK_NGX_Parameter_GetUI(parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &driver_minor);
        std::printf("DLSS Super Resolution: %s\n", available ? "available" : "not available");
        if (needs_driver) std::printf("a newer driver is needed: %u.%u or later\n", driver_major, driver_minor);
        if (available) {
            const struct {
                NVSDK_NGX_PerfQuality_Value mode;
                const char* name;
            } modes[] = {{NVSDK_NGX_PerfQuality_Value_DLAA, "DLAA"},
                         {NVSDK_NGX_PerfQuality_Value_MaxQuality, "Quality"},
                         {NVSDK_NGX_PerfQuality_Value_Balanced, "Balanced"},
                         {NVSDK_NGX_PerfQuality_Value_MaxPerf, "Performance"}};
            for (const auto& m : modes) {
                unsigned int width = 0, height = 0, max_width = 0, max_height = 0, min_width = 0, min_height = 0;
                float sharpness = 0;
                if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_OPTIMAL_SETTINGS(parameters, 2560, 1440, m.mode, &width, &height,
                                                                    &max_width, &max_height, &min_width, &min_height,
                                                                    &sharpness))) {
                    std::printf("  %-11s 2560x1440 from %ux%u\n", m.name, width, height);
                    if (m.mode != NVSDK_NGX_PerfQuality_Value_Balanced) {
                        bench_dlss(device.Get(), parameters, m.mode, m.name, width, height);
                    }
                }
            }
        }
        NVSDK_NGX_D3D11_DestroyParameters(parameters);
    } else {
        std::printf("no capability parameters: %08X\n", static_cast<unsigned>(result));
    }
    NVSDK_NGX_D3D11_Shutdown1(device.Get());
    return available ? 0 : 3;
}
#endif

}  // namespace

int wmain(int argc, wchar_t** argv) {
#ifdef XPORT_DLSS
    if (argc == 2 && std::wcscmp(argv[1], L"--probe") == 0) return probe_dlss();
#endif
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
