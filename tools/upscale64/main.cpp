// upscale64: the 64-bit helper of xport's upscale bridge (src/pc/upscale_bridge.h). It opens the textures the port
// shares by name (colour, depth, motion, output) and the frame's data, on the same graphics card, and turns each input
// into an output until the port's process ends.
//
//   upscale64.exe <port process ID> <adapter LUID high:low> <input w> <input h> <output w> <output h> [mode]
//   upscale64.exe --probe
//
// mode: bilinear (the default; any card), or dlaa, quality, balanced, performance: NVIDIA DLSS (NGX, 64-bit only),
// when built with the DLSS SDK (-DDLSS_SDK). A DLSS mode that cannot start ends the program with code 4.
// --probe: whether DLSS runs on the high-performance card, and its time on made-up frames.
// Plan: sick-xport docs/UPSCALING-DLSS-FSR.md.

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#ifdef XPORT_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring shared_name(DWORD process, const wchar_t* what) {  // as upscale_shared_name in upscale_bridge.cpp
    wchar_t name[96];
    std::swprintf(name, 96, L"Local\\xport-upscale-%lu-%ls", static_cast<unsigned long>(process), what);
    return name;
}

// As UpscaleFrame in upscale_bridge.h.
struct Frame {
    float jitter_x, jitter_y;
    int32_t reset;
    float motion_scale_x, motion_scale_y;
    uint32_t number;
};

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

// The bilinear stretch: the upscaler without DLSS.
struct Stretch {
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11SamplerState> sampler;

    bool make(ID3D11Device* device, ID3D11Texture2D* colour, ID3D11Texture2D* output) {
        const ComPtr<ID3DBlob> vs = compile("vs", "vs_5_0"), ps = compile("ps", "ps_5_0");
        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        return vs && ps && SUCCEEDED(device->CreateShaderResourceView(colour, nullptr, &source)) &&
               SUCCEEDED(device->CreateRenderTargetView(output, nullptr, &target)) &&
               SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_shader)) &&
               SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_shader)) &&
               SUCCEEDED(device->CreateSamplerState(&sampler_desc, &sampler));
    }

    void run(ID3D11DeviceContext* context, UINT width, UINT height) {
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
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
    }
};

#ifdef XPORT_DLSS
// NGX on `device`: it looks for nvngx_dlss.dll next to this program and writes its logs in %TEMP%\xport-upscale64.
bool start_ngx(ID3D11Device* device) {
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
    const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(
        "6f0c2b9e-31d4-4a55-9b6e-58d3c0a1e7f2", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "xport", logs.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(result)) std::fprintf(stderr, "upscale64: NGX did not start: %08X\n", static_cast<unsigned>(result));
    return NVSDK_NGX_SUCCEED(result);
}

const struct {
    const wchar_t* name;
    const char* title;
    NVSDK_NGX_PerfQuality_Value value;
} kModes[] = {{L"dlaa", "DLAA", NVSDK_NGX_PerfQuality_Value_DLAA},
              {L"quality", "Quality", NVSDK_NGX_PerfQuality_Value_MaxQuality},
              {L"balanced", "Balanced", NVSDK_NGX_PerfQuality_Value_Balanced},
              {L"performance", "Performance", NVSDK_NGX_PerfQuality_Value_MaxPerf}};

// DLSS for one size and mode, with the capability parameters it needs at each evaluation.
struct Dlss {
    ID3D11Device* device = nullptr;  // NGX started on it
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;

    ~Dlss() {
        if (feature) NVSDK_NGX_D3D11_ReleaseFeature(feature);
        if (parameters) NVSDK_NGX_D3D11_DestroyParameters(parameters);
        if (device) NVSDK_NGX_D3D11_Shutdown1(device);
    }

    bool start(ID3D11Device* d) {
        if (!start_ngx(d)) return false;
        device = d;
        int available = 0;
        if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters))) return false;
        NVSDK_NGX_Parameter_GetI(parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        if (!available) std::fprintf(stderr, "upscale64: DLSS is not available on this card\n");
        return available != 0;
    }

    bool make(ID3D11DeviceContext* context, NVSDK_NGX_PerfQuality_Value mode, UINT input_width, UINT input_height,
              UINT output_width, UINT output_height) {
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = input_width, create.Feature.InHeight = input_height;
        create.Feature.InTargetWidth = output_width, create.Feature.InTargetHeight = output_height;
        create.Feature.InPerfQualityValue = mode;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;  // vectors at the render size, LDR colour
        const NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(context, &feature, parameters, &create);
        if (NVSDK_NGX_FAILED(result)) std::fprintf(stderr, "upscale64: DLSS could not be made: %08X\n", static_cast<unsigned>(result));
        return NVSDK_NGX_SUCCEED(result);
    }

    bool run(ID3D11DeviceContext* context, ID3D11Texture2D* colour, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
             ID3D11Texture2D* output, UINT input_width, UINT input_height, const Frame& frame) {
        NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
        eval.Feature.pInColor = colour;
        eval.Feature.pInOutput = output;
        eval.pInDepth = depth;
        eval.pInMotionVectors = motion;
        eval.InJitterOffsetX = frame.jitter_x;
        eval.InJitterOffsetY = frame.jitter_y;
        eval.InRenderSubrectDimensions = {input_width, input_height};
        eval.InReset = frame.reset;
        eval.InMVScaleX = frame.motion_scale_x;
        eval.InMVScaleY = frame.motion_scale_y;
        return NVSDK_NGX_SUCCEED(NGX_D3D11_EVALUATE_DLSS_EXT(context, feature, parameters, &eval));
    }
};

float halton(int index, int base) {
    float f = 1, r = 0;
    for (int i = index; i > 0; i /= base) f /= base, r += f * (i % base);
    return r;
}

// DLSS made and run on made-up frames (a gradient, flat depth, no motion, the jitter a game would use): the time a
// frame takes on the GPU, from timestamp queries, for `mode` from width x height to 2560x1440.
void bench_dlss(ID3D11Device* device, Dlss& dlss, NVSDK_NGX_PerfQuality_Value mode, const char* name, unsigned width,
                unsigned height) {
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
        for (unsigned x = 0; x < width; ++x) {
            pixels[size_t{y} * width + x] = 0xFF000000u | ((x * 255 / width) << 16) | ((y * 255 / height) << 8) | ((x ^ y) & 0xFF);
        }
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
    if (dlss.feature) NVSDK_NGX_D3D11_ReleaseFeature(dlss.feature), dlss.feature = nullptr;
    if (!dlss.make(context.Get(), mode, width, height, 2560, 1440)) return;
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
        const Frame data{halton(frame % 16 + 1, 2) - 0.5f, halton(frame % 16 + 1, 3) - 0.5f, frame == 0, 1, 1,
                         static_cast<uint32_t>(frame)};
        context->Begin(disjoint.Get());
        context->End(start.Get());
        if (!dlss.run(context.Get(), colour.Get(), depth.Get(), motion.Get(), output.Get(), width, height, data)) ++failed;
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
}

// upscale64 --probe: whether DLSS Super Resolution runs on the high-performance graphics card (NGX on Direct3D 11),
// the render sizes it asks for a 2560x1440 output, and its time. 0: available, 3: not available, 2: NGX did not start.
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
    Dlss dlss;
    if (!start_ngx(device.Get())) return 2;
    dlss.device = device.Get();
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&dlss.parameters))) {
        std::printf("no capability parameters\n");
        return 2;
    }
    int available = 0, needs_driver = 0;
    unsigned int driver_major = 0, driver_minor = 0;
    NVSDK_NGX_Parameter_GetI(dlss.parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(dlss.parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(dlss.parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &driver_major);
    NVSDK_NGX_Parameter_GetUI(dlss.parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &driver_minor);
    std::printf("DLSS Super Resolution: %s\n", available ? "available" : "not available");
    if (needs_driver) std::printf("a newer driver is needed: %u.%u or later\n", driver_major, driver_minor);
    if (available) {
        for (const auto& m : kModes) {
            unsigned int width = 0, height = 0, max_width = 0, max_height = 0, min_width = 0, min_height = 0;
            float sharpness = 0;
            if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_OPTIMAL_SETTINGS(dlss.parameters, 2560, 1440, m.value, &width, &height,
                                                                &max_width, &max_height, &min_width, &min_height, &sharpness))) {
                std::printf("  %-11s 2560x1440 from %ux%u\n", m.title, width, height);
                if (m.value != NVSDK_NGX_PerfQuality_Value_Balanced) bench_dlss(device.Get(), dlss, m.value, m.title, width, height);
            }
        }
    }
    return available ? 0 : 3;
}
#endif

}  // namespace

int wmain(int argc, wchar_t** argv) {
#ifdef XPORT_DLSS
    if (argc == 2 && std::wcscmp(argv[1], L"--probe") == 0) return probe_dlss();
#endif
    if (argc != 7 && argc != 8) {
        std::fprintf(stderr, "usage: upscale64 <process> <luid high:low> <in w> <in h> <out w> <out h> [mode] | --probe\n");
        return 1;
    }
    const DWORD parent = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    LUID luid{};
    unsigned long high = 0, low = 0;
    if (std::swscanf(argv[2], L"%lx:%lx", &high, &low) != 2) return fail("bad adapter LUID", E_INVALIDARG);
    luid.HighPart = static_cast<LONG>(high), luid.LowPart = low;
    const UINT input_width = static_cast<UINT>(std::wcstoul(argv[3], nullptr, 10));
    const UINT input_height = static_cast<UINT>(std::wcstoul(argv[4], nullptr, 10));
    const UINT output_width = static_cast<UINT>(std::wcstoul(argv[5], nullptr, 10));
    const UINT output_height = static_cast<UINT>(std::wcstoul(argv[6], nullptr, 10));
    const wchar_t* mode = argc == 8 ? argv[7] : L"bilinear";
    const bool bilinear = std::wcscmp(mode, L"bilinear") == 0;
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

    struct Shared {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> lock;
    } colour, depth, motion, output;
    for (const auto& [what, shared] : {std::pair{L"colour", &colour}, std::pair{L"depth", &depth},
                                       std::pair{L"motion", &motion}, std::pair{L"output", &output}}) {
        hr = device->OpenSharedResourceByName(shared_name(parent, what).c_str(),
                                              DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                              IID_PPV_ARGS(&shared->texture));
        if (SUCCEEDED(hr)) hr = shared->texture.As(&shared->lock);
        if (FAILED(hr)) return fail("cannot open the shared textures", hr);
    }
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, shared_name(parent, L"frame").c_str());
    const auto* frame = mapping ? static_cast<const Frame*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Frame))) : nullptr;
    if (!frame) return fail("cannot open the frame's data", HRESULT_FROM_WIN32(GetLastError()));

    Stretch stretch;
#ifdef XPORT_DLSS
    Dlss dlss;
#endif
    if (bilinear) {
        if (!stretch.make(device.Get(), colour.texture.Get(), output.texture.Get())) return fail("cannot set up the stretch", E_FAIL);
    } else {
#ifdef XPORT_DLSS
        const auto chosen = std::find_if(std::begin(kModes), std::end(kModes), [&](const auto& m) { return std::wcscmp(m.name, mode) == 0; });
        if (chosen == std::end(kModes)) {
            std::fprintf(stderr, "upscale64: unknown mode %ls\n", mode);
            return 4;
        }
        if (!dlss.start(device.Get()) ||
            !dlss.make(context.Get(), chosen->value, input_width, input_height, output_width, output_height)) {
            return 4;
        }
#else
        std::fprintf(stderr, "upscale64: built without DLSS (-DDLSS_SDK)\n");
        return 4;
#endif
    }
    if (const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, shared_name(parent, L"ready").c_str())) {
        SetEvent(ready);
        CloseHandle(ready);
    }

    Shared* inputs[] = {&colour, &depth, &motion};
    for (;;) {
        // The inputs, or every half second a look at whether the port is still there.
        hr = colour.lock->AcquireSync(1, 500);
        if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
            if (WaitForSingleObject(port, 0) == WAIT_OBJECT_0) break;
            continue;
        }
        if (FAILED(hr) || depth.lock->AcquireSync(1, INFINITE) != S_OK || motion.lock->AcquireSync(1, INFINITE) != S_OK ||
            output.lock->AcquireSync(0, INFINITE) != S_OK) {
            break;
        }
        if (bilinear) {
            stretch.run(context.Get(), output_width, output_height);
        } else {
#ifdef XPORT_DLSS
            if (!dlss.run(context.Get(), colour.texture.Get(), depth.texture.Get(), motion.texture.Get(), output.texture.Get(),
                          input_width, input_height, *frame)) {
                std::fprintf(stderr, "upscale64: DLSS failed on frame %u\n", frame->number);
            }
#endif
        }
        context->Flush();
        output.lock->ReleaseSync(1);
        for (Shared* shared : inputs) shared->lock->ReleaseSync(0);
    }
    UnmapViewOfFile(frame);
    CloseHandle(mapping);
    CloseHandle(port);
    return 0;
}
