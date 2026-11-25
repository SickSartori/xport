#include "pc/renderer_d3d11.h"

#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <immintrin.h>
#include <type_traits>
#include <cmath>
#include <condition_variable>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "pc/log.h"
#include "pc/port.h"
#include "pc/paths.h"
#include "pc/physical_memory.h"
#include "pc/profile.h"
#include "pc/xbox_thread.h"

#pragma comment(lib, "synchronization.lib")  // WaitOnAddress
#include "pc/settings.h"
#include "pc/texture_page_hash.h"
#include "pc/texture_upscale.h"
#include "pc/upscale_bridge.h"

namespace xport::pc {

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kVertexShader[] = R"(
struct VSInput {
    float4 position : POSITION;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float fog : FOG;
};
struct PSInput {
    float4 position : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float fog : FOG;
};
// Positions arrive in coordinates of the render target times w; scale = (2 * pixels per coordinate / width, the same
// down, a game's narrowing across (renderer_game_narrowing), the coordinate column it narrows towards); scale_y = the
// same narrowing down (factor, row).
cbuffer Surface : register(b0) { float4 scale; float4 scale_y; };
PSInput main(VSInput i) {
    PSInput o;
    const float x = (i.position.x - scale.w * i.position.w) * scale.z + scale.w * i.position.w;
    const float y = (i.position.y - scale_y.y * i.position.w) * scale_y.x + scale_y.y * i.position.w;
    o.position = float4(x * scale.x - i.position.w, i.position.w - y * scale.y, i.position.z, i.position.w);
    o.d0 = i.d0;
    o.d1 = i.d1;
    o.t0 = i.t0;
    o.t1 = i.t1;
    o.t2 = i.t2;
    o.t3 = i.t3;
    o.fog = i.fog;
    return o;
}
)";

// The start of the vertex shaders that read the guest's vertex bytes as they are (fixed function and vertex
// programs): attribute decoding, fog factor and final position, the same computations as read_attribute,
// fog_factor and to_render_vertex in nv2a_kelvin.cpp (keep them in step). Each shader's own constants (b1) follow.
constexpr char kVertexDataShader[] = R"(
struct PSInput {
    float4 position : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float fog : FOG;
};
cbuffer Surface : register(b0) { float4 scale; float4 scale_y; };
cbuffer Draw : register(b2) { uint first; };  // byte offset of this draw's vertex data (changes every draw)
ByteAddressBuffer vertex_data : register(t0);

// From the bits rather than isnan, isinf and isfinite, which Wine's HLSL compiler lacks.
bool is_nan(float x) { return (asuint(x) & 0x7FFFFFFF) > 0x7F800000; }
bool is_inf(float x) { return (asuint(x) & 0x7FFFFFFF) == 0x7F800000; }
bool is_finite(float x) { return (asuint(x) & 0x7F800000) != 0x7F800000; }

uint load32(uint address) {  // attributes are not always 4-byte aligned (3 shorts)
    const uint aligned = address & ~3u, shift = (address & 3u) * 8;
    const uint low = vertex_data.Load(aligned);
    return shift ? (low >> shift) | (vertex_data.Load(aligned + 4) << (32 - shift)) : low;
}
float signed_bits(uint value, uint bits) {
    const int shifted = (int)(value << (32 - bits)) >> (32 - bits);
    return max(-1, shifted / (float)((1 << (bits - 1)) - 1));
}
// l: byte offset of vertex 0, stride, format, from the data (0: `fallback`, the attribute without an array).
float4 decode_attribute(uint vertex, int4 l, float4 fallback) {
    if (l.w == 0) return fallback;
    const uint address = (uint)((int)first + l.x + (int)vertex * l.y);
    const uint type = l.z & 15, size = (l.z >> 4) & 15;
    float4 decoded = float4(0, 0, 0, 1);
    if (type == 0 || type == 4) {  // UB_D3D (B G R A in memory), UB_OGL
        const uint w = load32(address);
        decoded = float4(w & 255, (w >> 8) & 255, (w >> 16) & 255, w >> 24) / 255;
        if (type == 0 && size >= 4) return decoded.zyxw;
    } else if (type == 1 || type == 5) {  // S1 (normalised), S32K
        const uint w0 = load32(address), w1 = size > 2 ? load32(address + 4) : 0;
        decoded = float4((int)(w0 << 16) >> 16, (int)w0 >> 16, (int)(w1 << 16) >> 16, (int)w1 >> 16);
        if (type == 1) decoded = max(-1, decoded / 32767);
    } else if (type == 2) {  // F
        [unroll] for (uint i = 0; i < 4; ++i) decoded[i] = i < size ? asfloat(load32(address + i * 4)) : 0;
    } else if (type == 6) {  // CMP: 11, 11, 10 signed bits
        const uint p = load32(address);
        return float4(signed_bits(p & 0x7FF, 11), signed_bits((p >> 11) & 0x7FF, 11), signed_bits(p >> 22, 10), 1);
    } else {
        return float4(0, 0, 0, 1);
    }
    float4 v = float4(0, 0, 0, 1);
    [unroll] for (uint i = 0; i < 4; ++i) if (i < size) v[i] = decoded[i];
    return v;
}

float fog_factor(float distance, bool enabled, uint mode, float4 params) {
    if (!enabled) return 1;
    if (is_inf(distance)) return mode == 0x801 || mode == 0x803 ? 0 : 1;
    float factor = 1;
    if (mode == 0x2601 || mode == 0x804) factor = params.x + distance * params.y - 1;
    else if (mode == 0x800 || mode == 0x802) factor = params.x + exp2(distance * params.y * 16) - 1.5;
    else if (mode == 0x801 || mode == 0x803) factor = params.x + exp2(-distance * distance * params.y * params.y * 32) - 1.5;
    if (mode == 0x802 || mode == 0x803 || mode == 0x804) factor = abs(factor);
    return is_nan(factor) ? 1 : factor;
}

// A screen-space position (pixels; w apart) to clip space, as to_render_vertex and then the main vertex shader do.
// precise, here and in the vertex programs: the compiler may not fuse or reorder, so two draws of the same mesh with
// different programs (a second pass with LEQUAL and no depth write, a stencil shadow volume) get the same depth.
float4 clip_position(float4 screen, float clip_max) {
    precise float w = screen.w;
    if (!is_finite(w) || abs(w) < 5.42101e-20) w = w < 0 ? -5.42101e-20 : 5.42101e-20;
    precise const float z = clip_max != 0 ? screen.z / clip_max : screen.z;
    // The NV2A keeps 4 fractional bits of a vertex's screen position (xemu's roundScreenCoords): nearly equal
    // vertices of two meshes (a building's edge and its shadow volume's) land on the same spot, with no gap between.
    precise const float x = (trunc(screen.x * 16) / 16 - scale.w) * scale.z + scale.w;
    precise const float y = (trunc(screen.y * 16) / 16 - scale_y.y) * scale_y.x + scale_y.y;
    precise const float4 clip = float4(x * w * scale.x - w, w - y * w * scale.y, z * w, w);
    return clip;
}
)";

// The fixed-function transform on the GPU: the same computations as transform_fixed_function and light_vertex in
// nv2a_kelvin.cpp (keep them in step). Follows kVertexDataShader.
constexpr char kFixedFunctionVertexShader[] = R"(
cbuffer FixedFunction : register(b1) {
    float4 composite[4];
    float4 model_view[4];
    float4 inverse_model_view[4];
    float4 texture_matrix[16];
    float4 texgen_plane[16];
    int4 texgen_mode[4];
    float4 attribute_default[16];
    int4 attribute_layout[16];  // byte offset of vertex 0, stride, format, from the data
    float4 light[64];
    float4 viewport_offset;
    float4 eye_position;
    float4 scene_ambient;
    float4 material_emission;
    float4 specular_params[2];
    float4 fog_plane;
    float4 fog_params;
    uint4 settings;  // flags, colour material, light enable mask, fog generation mode
    uint fog_mode;
    uint texture_matrix_mask;
    uint padding;
    float clip_max;
};

static const uint kLighting = 1, kSpecular = 2, kFog = 4, kNormalize = 8, kSeparateSpecular = 16, kLocalEye = 32;

float4 attribute(uint vertex, uint a) { return decode_attribute(vertex, attribute_layout[a], attribute_default[a]); }
float4 rows4(float4 r0, float4 r1, float4 r2, float4 r3, float4 v) {
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}
float light_value(uint i, uint k) { return light[i * 8 + k / 4][k % 4]; }
float3 light_vector(uint i, uint k) { return float3(light_value(i, k), light_value(i, k + 1), light_value(i, k + 2)); }
float specular_value(uint k) { return specular_params[k / 4][k % 4]; }
float3 safe_normalize(float3 v) { const float l = length(v); return l > 0 ? v / l : v; }

PSInput main(uint vertex : SV_VertexID) {
    const float4 position = attribute(vertex, 0);
    const float4 normal_in = attribute(vertex, 2);
    const float4 diffuse = attribute(vertex, 3);
    const float4 specular = attribute(vertex, 4);
    const float4 eye = rows4(model_view[0], model_view[1], model_view[2], model_view[3], position);
    precise const float4 p = rows4(composite[0], composite[1], composite[2], composite[3], position);
    precise float w = p.w;
    if (!is_finite(w) || abs(w) < 5.42101e-20) w = w < 0 ? -5.42101e-20 : 5.42101e-20;
    precise const float3 screen = float3(p.x / w + viewport_offset.x, p.y / w + viewport_offset.y, p.z / w);

    float4 out_diffuse, out_specular;
    if (settings.x & kLighting) {
        const uint material = settings.y;
        const uint emission_source = material & 3, ambient_source = (material >> 2) & 3,
                   diffuse_source = (material >> 4) & 3, specular_source = (material >> 6) & 3;
        const float3 emission_factor = material_emission.xyz;
        float3 front;
        if (emission_source == 0 || ambient_source == 0) {
            front = scene_ambient.xyz;
            if (emission_source != 0) front += (emission_source == 1 ? diffuse.rgb : specular.rgb) * emission_factor;
            if (ambient_source != 0) front += (ambient_source == 1 ? diffuse.rgb : specular.rgb) * emission_factor;
        } else {
            front = (emission_source == 1 ? diffuse.rgb : specular.rgb) +
                    (ambient_source == 1 ? diffuse.rgb : specular.rgb) * emission_factor;
        }
        const float alpha = diffuse_source == 0 ? material_emission.w : diffuse_source == 1 ? diffuse.a : specular.a;
        float3 lit_specular = 0;
        float3 normal = rows4(inverse_model_view[0], inverse_model_view[1], inverse_model_view[2],
                              inverse_model_view[3], float4(normal_in.xyz, 0)).xyz;
        if (settings.x & kNormalize) normal = safe_normalize(normal);
        const float ew = eye.w != 0 ? eye.w : 1;
        const float3 eye_point = eye.xyz / ew;
        const bool local_eye = (settings.x & kLocalEye) != 0;
        const float3 to_eye = safe_normalize(eye_position.xyz - eye_point);
        [loop] for (uint i = 0; i < 8; ++i) {
            const uint type = (settings.z >> (i * 2)) & 3;
            if (type == 0) continue;
            float attenuation = 1;
            float3 direction, half_vector;
            if (type == 1) {
                direction = safe_normalize(light_vector(i, 13));
                half_vector = local_eye ? safe_normalize(direction + to_eye) : light_vector(i, 10);
            } else {
                const float3 to_light = light_vector(i, 23) - eye_point;
                const float d = length(to_light);
                if (d > light_value(i, 9)) continue;
                direction = safe_normalize(to_light);
                const float denominator = light_value(i, 26) + light_value(i, 27) * d + light_value(i, 28) * d * d;
                attenuation = denominator != 0 ? 1 / denominator : 1;
                half_vector = safe_normalize(direction + (local_eye ? to_eye : float3(0, 0, 0)));
                if (type == 3) {
                    const float cosine = light_value(i, 19) * direction.x + light_value(i, 20) * direction.y +
                                         light_value(i, 21) * direction.z + light_value(i, 22);
                    float spot = 1;
                    if (cosine <= 0) {
                        spot = 0;
                    } else if (cosine < 1 && cosine > -light_value(i, 16)) {
                        const float denom = light_value(i, 17) * cosine + light_value(i, 18);
                        spot = denom != 0 ? saturate((cosine + light_value(i, 16)) / denom) : 1;
                    }
                    attenuation *= spot;
                }
            }
            const float n_dot_l = max(0, dot(normal, direction));
            const float n_dot_h = max(0, dot(normal, half_vector));
            float power = 0;
            if (n_dot_l > 0 && n_dot_h > -specular_value(0)) {
                const float denominator = specular_value(1) * n_dot_h + specular_value(2);
                power = denominator == 0 ? 10000 : denominator > 0 ? clamp((n_dot_h + specular_value(0)) / denominator, 0, 10000) : 0;
            }
            front += light_vector(i, 0) * attenuation;
            float3 diffuse_term = light_vector(i, 3) * (attenuation * n_dot_l);
            if (diffuse_source != 0) diffuse_term *= diffuse_source == 1 ? diffuse.rgb : specular.rgb;
            front += diffuse_term;
            float3 specular_term = light_vector(i, 6) * (attenuation * power);
            if (specular_source != 0) specular_term *= specular_source == 1 ? diffuse.rgb : specular.rgb;
            lit_specular += specular_term;
        }
        out_diffuse = float4(front, alpha);
        if ((settings.x & kSpecular) == 0) {
            out_specular = float4(0, 0, 0, 1);
        } else if (settings.x & kSeparateSpecular) {
            out_specular = float4(lit_specular, specular.a);
        } else {
            out_diffuse = float4(front + lit_specular, alpha);
            out_specular = specular;
        }
    } else {
        out_diffuse = diffuse;
        out_specular = (settings.x & kSpecular) ? specular : float4(0, 0, 0, 1);
    }

    float4 textures[4];
    [unroll] for (uint stage = 0; stage < 4; ++stage) {
        float4 t = attribute(vertex, 9 + stage);
        [unroll] for (uint c = 0; c < 4; ++c) {
            const int mode = texgen_mode[stage][c];
            if (mode == 1) t[c] = dot(texgen_plane[stage * 4 + c], eye);
            else if (mode == 2) t[c] = dot(texgen_plane[stage * 4 + c], position);
        }
        if (texture_matrix_mask & (1u << stage)) {
            t = rows4(texture_matrix[stage * 4], texture_matrix[stage * 4 + 1], texture_matrix[stage * 4 + 2],
                      texture_matrix[stage * 4 + 3], t);
        }
        textures[stage] = t;
    }

    float fog = 0;
    if (settings.x & kFog) {
        const float planar = dot(fog_plane.xyz, eye.xyz) + fog_plane.w;
        const uint generation = settings.w;
        if (generation == 0) fog = saturate(specular.a);
        else if (generation == 1) fog = length(eye.xyz);
        else if (generation == 2) fog = planar;
        else if (generation == 3) fog = abs(planar);
        else if (generation == 6) fog = attribute(vertex, 5).x;
    }

    PSInput o;
    o.position = clip_position(float4(screen, w), clip_max);
    o.d0 = out_diffuse;
    o.d1 = out_specular;
    o.t0 = textures[0];
    o.t1 = textures[1];
    o.t2 = textures[2];
    o.t3 = textures[3];
    o.fog = fog_factor(fog, (settings.x & kFog) != 0, fog_mode, fog_params);
    return o;
}
)";

// The start of a vertex program's shader (follows kVertexDataShader); the program's own code, generated by
// VertexProgram::hlsl, comes after. The helpers mirror the special cases of nv2a_vsh.cpp.
constexpr char kVertexProgramShader[] = R"(
cbuffer Program : register(b1) {
    float4 c[192];
    float4 attribute_default[16];
    int4 attribute_layout[16];
    float4 fog_params;
    uint fog_enabled;
    uint fog_mode;
    uint padding;
    float clip_max;
};

static const float kInfinity = asfloat(0x7F800000);

float4 attribute(uint vertex, uint a) { return decode_attribute(vertex, attribute_layout[a], attribute_default[a]); }
// 0 * anything is 0 on NV2A, even for infinities, which programs rely on.
float4 mul0(float4 a, float4 b) { return (a == 0 || b == 0) ? 0 : a * b; }
float4 rcc(float s) {
    const float r = 1 / s;
    if (is_nan(r)) return 5.42101e-20;
    const float magnitude = clamp(abs(r), 5.42101e-20, 1.884467e+19);
    return (asuint(r) >> 31) ? -magnitude : magnitude;
}
float4 rsq(float s) { return s == 0 ? kInfinity : is_inf(s) ? 0 : 1 / sqrt(abs(s)); }
float4 exp_ilu(float s) { const float f = floor(s); return float4(exp2(f), s - f, exp2(s), 1); }
float4 log_ilu(float s) {
    const float t = abs(s);
    if (t == 0) return float4(-kInfinity, 1, -kInfinity, 1);
    const float e = floor(log2(t));
    return float4(e, t / exp2(e), log2(t), 1);
}
float4 lit(float4 v) {
    const float epsilon = 1.0 / 256;
    const float x = max(v.x, 0), y = max(v.y, 0), w = clamp(v.w, -(128 - epsilon), 128 - epsilon);
    return float4(1, x, x > 0 ? exp2(w * log2(y)) : 0, 1);
}
)";

struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;  // R16G16B16A16_FLOAT once a signed blend equation is used
};

struct DepthTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;  // the depth, read by the ambient occlusion pass
    uint32_t width = 0, height = 0;
};

// Setting sharp_textures enlarges the scene's textures, over a thousand at once in a city: the bytes of the enlarged
// ones alive, kept within a budget (a 32-bit program, and graphics cards with 2 GB). Counted down when its texture goes.
uint64_t g_enlarged_bytes = 0;
constexpr uint64_t kEnlargedBudget = 768ull << 20;
struct EnlargedBytes {
    uint32_t bytes = 0;
    EnlargedBytes() = default;
    EnlargedBytes(EnlargedBytes&& other) noexcept : bytes(std::exchange(other.bytes, 0)) {}
    EnlargedBytes& operator=(EnlargedBytes&& other) noexcept {
        g_enlarged_bytes -= bytes;
        bytes = std::exchange(other.bytes, 0);
        return *this;
    }
    ~EnlargedBytes() { g_enlarged_bytes -= bytes; }
    void set(uint32_t now) {
        g_enlarged_bytes += now;
        g_enlarged_bytes -= bytes;
        bytes = now;
    }
};

struct CachedTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint64_t content_hash = 0;
    uint32_t checked_generation = 0;  // memory unchanged since this write-tracking generation
    TexturePageHashes page_hashes;
    bool waiting = false;            // sharp_2d, sharp_textures: the enlarged version is being made (Enlarger)
    EnlargedBytes enlarged;          // what the enlarged version takes
    uint64_t last_used = 0;          // g_texture_clock when a draw last asked for it
};

// By address, format, pitch, rectangle, palette and whether it is enlarged (settings sharp_2d and sharp_textures).
using TextureKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>;

// sharp_2d: textures are enlarged on a thread of their own (texture_upscale.cpp), the original showing meanwhile:
// enlarging a large picture all at once (MC3's radar map at the start of a race) held the game up for a third of a
// second.
class Enlarger {
public:
    // `scene`: a texture of the 3D scene. Those come by the hundred on entering a place, so they get more threads,
    // below the game's in priority; the HUD's go first.
    void request(const TextureKey& key, uint64_t hash, const TextureImage& image, bool scene,
                 const std::vector<TextureImage>& lower) {
        std::lock_guard<std::mutex> lock(mutex_);
        const unsigned wanted = scene ? std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u) : 1u;
        for (; started_ < wanted; ++started_) {
            std::thread([this] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                run();
            }).detach();
        }
        if (scene) queue_.push_back({key, hash, image, scene, lower});
        else queue_.push_front({key, hash, image, scene, {}});
        wake_.notify_one();
    }
    // True once the texture `key` with content `hash` is done; `levels` empty when it was not worth enlarging. A
    // result for content since replaced is dropped (a new one is on its way).
    bool take(const TextureKey& key, uint64_t hash, std::vector<TextureImage>& levels) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = done_.find(key);
        if (found == done_.end()) return false;
        const bool current = found->second.first == hash;
        if (current) levels = std::move(found->second.second);
        done_.erase(found);
        return current;
    }
    // Drops the finished results nobody will take (`wanted` false), such as those for textures forgotten meanwhile.
    template <typename Wanted>
    void drop_unwanted(Wanted wanted) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = done_.begin(); it != done_.end();) it = wanted(it->first) ? std::next(it) : done_.erase(it);
    }

private:
    struct Job {
        TextureKey key;
        uint64_t hash;
        TextureImage image;
        bool scene = false;
        std::vector<TextureImage> lower;  // the game's own lower levels (scene)
    };
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return !queue_.empty(); });
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            std::vector<TextureImage> levels = upscale_texture(job.image, job.scene, job.lower);
            std::lock_guard<std::mutex> lock(mutex_);
            done_[job.key] = {job.hash, std::move(levels)};
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::map<TextureKey, std::pair<uint64_t, std::vector<TextureImage>>> done_;
    unsigned started_ = 0;  // threads
};
Enlarger g_enlarger;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11DeviceContext1> g_context1;
ComPtr<IDXGISwapChain1> g_swap_chain;
UINT g_swap_flags = 0;  // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING where DXGI offers it: Present never waits for the display
uint32_t g_swap_width = 0, g_swap_height = 0;
ComPtr<ID3D11VertexShader> g_vertex_shader;
ComPtr<ID3D11InputLayout> g_input_layout;
ComPtr<ID3D11VertexShader> g_fixed_function_vertex_shader;  // null: the transform stays on the CPU
ComPtr<ID3D11Buffer> g_fixed_function_constants;
ComPtr<ID3D11Buffer> g_fixed_function_draw_constants;  // where the draw's vertex data starts
ComPtr<ID3D11Buffer> g_program_constants;              // the vertex programs' constants (ProgramConstants)
ComPtr<ID3D11Buffer> g_vertex_buffer;
uint32_t g_vertex_capacity = 0;
uint32_t g_vertex_used = 0;  // bytes of the ring buffer written since the last DISCARD
ComPtr<ID3D11Buffer> g_constants;
ComPtr<ID3D11RasterizerState> g_rasterizer;  // no culling (clears, copies)
std::map<std::tuple<D3D11_CULL_MODE, bool, int, float>, ComPtr<ID3D11RasterizerState>> g_cull_rasterizers;  // + depth bias, slope

// <GAME>_FLIP_WINDING=1 inverts how NV2A's front-face winding maps onto D3D11 (for checking the convention).
const bool g_flip_winding = GetEnvironmentVariableW(XPORT_ENV_W("FLIP_WINDING"), nullptr, 0) != 0;

// Internal resolution: every render target is this many times the game's size in each direction, and drawn
// scaled (positions arrive relative to the game's size, so only viewports, scissors, clears and copies change).
// The picture is reduced to the window when presented: smoother edges and sharper detail (setting render_scale,
// read when the renderer starts). It need not be whole: 1080 lines are 2.25 times the game's 480.
float g_scale = 1;

ID3D11RasterizerState* rasterizer_for(const DrawState& state) {
    // Polygon offset (NV2A: scale * slope + bias * smallest depth step; D3D11's D24 DepthBias counts the same steps).
    // The slope is the depth change per pixel, so at g_scale times the pixels it is g_scale times smaller: the game's
    // scale is multiplied back, or the offset falls short on surfaces seen at an angle (Spider-Man 2's decals).
    static const bool off = std::getenv(XPORT_ENV("NO_POLY_OFFSET")) != nullptr;  // debugging: ignore the game's offset
    const int bias = state.offset_fill && !off ? static_cast<int>(std::lround(state.offset_bias)) : 0;
    const float slope = state.offset_fill && !off ? state.offset_scale * g_scale : 0.0f;
    if (!state.cull_enable && !bias && slope == 0.0f) return g_rasterizer.Get();
    constexpr uint32_t kCullFront = 0x404, kCullBack = 0x405, kFrontCcw = 0x901;
    const D3D11_CULL_MODE mode = !state.cull_enable            ? D3D11_CULL_NONE
                                 : state.cull_face == kCullFront  ? D3D11_CULL_FRONT
                                 : state.cull_face == kCullBack ? D3D11_CULL_BACK
                                                                : D3D11_CULL_NONE;  // FRONT_AND_BACK: see below
    // Positions keep NV2A's screen orientation (y down) through clip space and the viewport, so the winding the
    // game states is the winding D3D11 sees.
    const bool front_ccw = (state.front_face == kFrontCcw) != g_flip_winding;
    ComPtr<ID3D11RasterizerState>& cached = g_cull_rasterizers[{mode, front_ccw, bias, slope}];
    if (!cached) {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = mode;
        desc.FrontCounterClockwise = front_ccw;
        desc.DepthBias = bias;
        desc.SlopeScaledDepthBias = slope;
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = TRUE;
        g_device->CreateRasterizerState(&desc, &cached);
        log_line("renderer: rasterizer cull %d ccw %d depth bias %d slope %g", static_cast<int>(mode), front_ccw, bias, slope);
    }
    return cached.Get();
}
ComPtr<ID3D11Buffer> g_surface_constants;  // vertex shader: pixel-to-clip scale of the current target
uint32_t g_surface_scale_width = 0, g_surface_scale_height = 0, g_surface_scale_pixels_x = 0, g_surface_scale_pixels_y = 0;
float g_surface_scale_narrow = 1, g_surface_scale_anchor = 0;  // renderer_game_narrowing
float g_surface_scale_narrow_y = 1, g_surface_scale_anchor_y = 0;
ComPtr<ID3D11VertexShader> g_clear_vertex_shader;
ComPtr<ID3D11PixelShader> g_clear_pixel_shader;
ComPtr<ID3D11Buffer> g_clear_constants;
std::map<uint32_t, ComPtr<ID3D11BlendState>> g_clear_blend_states;  // by write mask
ComPtr<ID3D11Texture2D> g_upload;  // guest framebuffers that were never rendered to
ComPtr<ID3D11ShaderResourceView> g_upload_srv;
uint32_t g_upload_width = 0, g_upload_height = 0;

// Presentation: the frame is drawn into the window keeping the game's 4:3 shape, black bars around it.
HWND g_window_handle = nullptr;
// The finished window picture (effects and overlay), drawn by the GPU thread; a thread of its own copies it into the
// swap chain and presents it, so a frame limiter or overlay tool (RTSS) waiting inside Present holds up that thread
// only, not the emulated GPU (issues #3, #6).
ComPtr<ID3D11Texture2D> g_final_texture, g_back_buffer;
ComPtr<ID3D11RenderTargetView> g_final_rtv;
// The swap chain has a device of its own on the same adapter, used by the present thread only. An overlay hooked on
// Present (RTSS, Steam, Discord) draws with the swap chain's device: on the game's device it rebound targets and
// textures under the GPU thread's draws, and the city flickered and lost its textures (issue #6). The final picture
// is shared between the two devices, a keyed mutex (key 0 on both sides) ordering drawing and copying. Without the
// second device these are the game's own and the GPU thread presents.
ComPtr<ID3D11Device> g_present_device;
ComPtr<ID3D11DeviceContext> g_present_context;
ComPtr<ID3D11Texture2D> g_present_source;              // g_final_texture as g_present_device sees it
ComPtr<IDXGIKeyedMutex> g_final_lock, g_present_lock;  // the shared picture's mutex, from each side
std::mutex g_present_mutex;  // the final picture: being drawn or being copied
std::mutex g_swap_mutex;     // the swap chain: presenting or resizing
std::condition_variable g_present_wake;
bool g_present_pending = false, g_present_threaded = false;
ComPtr<ID3D11VertexShader> g_present_vertex_shader;
ComPtr<ID3D11PixelShader> g_present_pixel_shader;
ComPtr<ID3D11SamplerState> g_present_sampler;

std::unordered_map<uint32_t, Target> g_targets;
std::unordered_map<uint64_t, DepthTarget> g_depth_targets;
std::unordered_set<uint32_t> g_zeta_addresses;   // memory used as a depth buffer
std::unordered_set<uint32_t> g_colour_over_zeta;  // ... that a draw has since drawn colour into (colour_to_depth)
std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> g_pixel_shaders;
std::map<TextureKey, CachedTexture> g_textures;
uint64_t g_texture_clock = 0;  // frames shown, for forgetting the textures no draw asks for any more

// What find_texture_view answered for the textures last asked for, by descriptor: a draw's textures are nearly always
// among those of the draws before. While the game has not written the texture's memory since it was last checked
// (the same test find_texture_view makes first) the answer stands: asking again was two table lookups, a search in
// the map of textures and the texture's size worked out again, for every stage of every draw. Not for a texture still
// being enlarged (its entry is polled), nor once a render target or a depth copy has been made since (the address
// may now be one: they are only ever added). forget_unused_textures drops these with the entries.
struct ViewMemo {
    CachedTexture* cached = nullptr;
    TextureDescriptor descriptor{};
    bool flat = false;
    uint32_t bytes = 0, palette = 0;  // what find_texture_view checks for writes
    uint32_t generation = 0;          // g_write_generation when the answer was last confirmed
    size_t targets = 0, depth_colours = 0;
};
ViewMemo g_view_memo[512];

// Every 10 s of frames: the textures unused for a minute are forgotten (the game reuses its memory for new ones, and
// each kept its GPU copy for ever). Called while presenting, when no CachedTexture& is held and the bound state is
// forgotten anyway (ForgetBound).
void forget_unused_textures() {
    constexpr uint64_t kSweepFrames = 600, kUnusedFrames = 3600;
    if (++g_texture_clock % kSweepFrames) return;
    for (ViewMemo& memo : g_view_memo) memo = ViewMemo{};
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        it = g_texture_clock - it->second.last_used > kUnusedFrames ? g_textures.erase(it) : std::next(it);
    }
    g_enlarger.drop_unwanted([](const TextureKey& key) {
        const auto found = g_textures.find(key);
        return found != g_textures.end() && found->second.waiting;
    });
}
std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, ComPtr<ID3D11BlendState>> g_blend_states;
std::map<std::tuple<bool, uint32_t, bool, bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>,
         ComPtr<ID3D11DepthStencilState>>
    g_depth_states;
std::map<std::pair<uint32_t, uint32_t>, ComPtr<ID3D11SamplerState>> g_samplers;

// Statistics, logged every 10 s from renderer_present: where the GPU thread's time goes.
struct Statistics {
    uint64_t presents = 0, draws = 0, vertices = 0, vertex_ticks = 0;
    uint64_t hashed_bytes = 0, hash_ticks = 0, uploads = 0, upload_ticks = 0, draw_ticks = 0;
    // Parts of draw_ticks: target/shader lookup, vertex upload, state, textures (with state when not apart), the
    // Draw call.
    uint64_t part_ticks[5] = {};
    uint64_t swap_ticks = 0;  // in Present: long when the graphics card is behind
    uint64_t shader_waits = 0;  // draws left out while their combiner shader compiles (pixel_shader)
} g_stats;
ULONGLONG g_stats_since = 0;

uint64_t ticks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

double milliseconds(uint64_t t) {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return t * 1000.0 / frequency;
}

uint64_t g_total_draws = 0;
bool g_queue_alternates = false;  // setting render_thread 2
bool g_queue_on = false;  // draws and clears go to the render thread (see "the render thread" below)
// For the log: how often and how long the GPU thread waited for the render thread, to draw what was queued (flushes)
// and for room in the queue.
struct QueueWaits {
    uint32_t flushes = 0, waited_flushes = 0;
    uint64_t flush_ticks = 0, full_ticks = 0;
};
QueueWaits g_queue_waits;

void log_statistics() {
    const ULONGLONG now = GetTickCount64();
    if (!g_stats_since) g_stats_since = now;
    if (now - g_stats_since < 10000) return;
    const double seconds = (now - g_stats_since) / 1000.0;
    log_line("renderer: %.1f fps, %.0f draws/s, %.0f vertices/s | per second: vertices %.0f ms, texture hash %.0f ms "
             "(%.1f MB), %.0f uploads %.0f ms, D3D %.0f ms; %zu textures kept; on CPU %lu",
             g_stats.presents / seconds, g_stats.draws / seconds, g_stats.vertices / seconds,
             milliseconds(g_stats.vertex_ticks) / seconds, milliseconds(g_stats.hash_ticks) / seconds,
             g_stats.hashed_bytes / seconds / 1048576.0, g_stats.uploads / seconds,
             milliseconds(g_stats.upload_ticks) / seconds, milliseconds(g_stats.draw_ticks) / seconds,
             g_textures.size(), GetCurrentProcessorNumber());  // hybrid CPUs: which core this thread got
    log_line("renderer: D3D parts per second: setup %.0f ms, vertex upload %.0f ms, state and textures %.0f ms, "
             "draw %.0f ms; Present %.0f ms; %llu draws waited for a shader",
             milliseconds(g_stats.part_ticks[0]) / seconds, milliseconds(g_stats.part_ticks[1]) / seconds,
             milliseconds(g_stats.part_ticks[2] + g_stats.part_ticks[3]) / seconds,
             milliseconds(g_stats.part_ticks[4]) / seconds, milliseconds(g_stats.swap_ticks) / seconds,
             static_cast<unsigned long long>(g_stats.shader_waits));
    if (g_queue_on) {
        log_line("renderer: render thread: the GPU thread waited %.0f ms a second for it in %.0f of %.0f flushes, %.0f ms for "
                 "room in the queue",
                 milliseconds(g_queue_waits.flush_ticks) / seconds, g_queue_waits.waited_flushes / seconds,
                 g_queue_waits.flushes / seconds, milliseconds(g_queue_waits.full_ticks) / seconds);
        g_queue_waits = {};
    }
    g_stats = {};
    g_stats_since = now;
}

uint64_t hash_memory(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, 8);
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

// Only to see whether memory changed: four independent 32-bit lanes, which a 32-bit build multiplies in one
// instruction each (hash_memory's 64-bit multiply is a library call here); about three times faster.
uint64_t change_hash(const uint8_t* data, size_t size) {
    uint32_t lane[4] = {0x811C9DC5u, 0x01000193u, 0x9E3779B1u, 0x85EBCA6Bu};
    size_t i = 0;
    for (; i + 16 <= size; i += 16) {
        uint32_t words[4];
        std::memcpy(words, data + i, 16);
        for (int k = 0; k < 4; ++k) lane[k] = (lane[k] ^ words[k]) * 0x01000193u;
    }
    for (; i < size; ++i) lane[i & 3] = (lane[i & 3] ^ data[i]) * 0x01000193u;
    const uint32_t low = lane[0] ^ (lane[2] * 0x9E3779B1u), high = lane[1] ^ (lane[3] * 0x85EBCA6Bu);
    return (static_cast<uint64_t>(high) << 32 | low) ^ size;
}

ComPtr<ID3DBlob> compile(const char* source, size_t size, const char* profile, const char* name) {
    ComPtr<ID3DBlob> code, errors;
    ProfileScope scope(ProfileTime::shader_compile);
    const HRESULT hr = D3DCompile(source, size, name, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1,
                                  0, &code, &errors);
    if (FAILED(hr)) {
        log_line("renderer: %s failed to compile: %s", name,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

// Internal resolution (g_scale, above): texture sizes, scissors and copies round with scaled(), the same everywhere,
// so that a colour and a depth target of the same game size always match; viewports keep the exact value.
LONG scaled(uint32_t pixels) { return std::lround(static_cast<float>(pixels) * g_scale); }
uint32_t scaled_size(uint32_t pixels) { return static_cast<uint32_t>(scaled(pixels)); }
float scaled_float(uint32_t pixels) { return static_cast<float>(pixels) * g_scale; }

// The graphics card stopped answering (a driver crash or update, a GPU overloaded): nothing drawn from here on would
// show. <port> says so and starts again, once: a second loss in a row is not one a restart mends.
[[noreturn]] void device_lost(HRESULT hr) {
    constexpr wchar_t kRestarted[] = XPORT_ENV_W("RESTARTED_AFTER_GPU_LOSS");
    const HRESULT reason = g_device ? g_device->GetDeviceRemovedReason() : hr;
    log_line("renderer: the graphics card stopped responding: %08lX, reason %08lX", hr, reason);
    const bool restart = !GetEnvironmentVariableW(kRestarted, nullptr, 0);
    // No box in RecompLauncher's Big Picture mode: behind its full screen a controller could not close it.
    wchar_t launcher_mode[16] = {};
    GetEnvironmentVariableW(L"RECOMPLAUNCHER_MODE", launcher_mode, 16);
    if (!GetEnvironmentVariableW(XPORT_ENV_W("NO_DIALOG"), nullptr, 0) && wcscmp(launcher_mode, L"bigpicture") != 0) {
        MessageBoxA(nullptr,
                    restart ? "The graphics card stopped responding, so the game has to start again. Progress since "
                              "your last save is lost.\n\nIf it happens again, update the graphics driver or lower "
                              "the resolution in the settings."
                            : "The graphics card stopped responding again. Update the graphics driver or lower the "
                              "resolution in the settings, then start " XPORT_NAME " again.",
                    XPORT_NAME, MB_ICONERROR | MB_OK);
    }
    // Started by RecompLauncher, <port>.exe stays as the process it watches and starts the next one itself (the port's
    // restart_codes.h: <PORT>_EXIT_RESTART_AFTER_GPU_LOSS, with RESTARTED_AFTER_GPU_LOSS set).
    if (restart && GetEnvironmentVariableW(XPORT_ENV_W("SUPERVISED"), nullptr, 0)) {
        log_line("renderer: starting again through " XPORT_NAME "'s supervisor");
        AllowSetForegroundWindow(ASFW_ANY);
        end_process(0x52530004);
    }
    if (restart) {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            SetEnvironmentVariableW(kRestarted, L"1");
            // The new process waits for this one before it touches the log, which says why it restarted.
            SetEnvironmentVariableW(XPORT_ENV_W("RESTART_FROM"), std::to_wstring(GetCurrentProcessId()).c_str());
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            std::wstring command = GetCommandLineW();
            if (CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            } else {
                log_line("renderer: could not start again: error %lu", GetLastError());
            }
        }
    }
    end_process(1);
}

// A failed creation used to pass silently and leave a hole in the picture; the log now says what was asked for,
// since a driver may refuse a format or a size that others accept. A few lines are enough: the first ones tell.
void log_resource_failure(const char* what, HRESULT hr, const D3D11_TEXTURE2D_DESC& desc) {
    static std::atomic<int> reported{0};
    if (reported++ >= 20) return;
    log_line("renderer: cannot create %s: error %08lX (%ux%u, %u levels, %u slices, format %u, usage %u, bind %X, "
             "cpu %X, misc %X)",
             what, hr, desc.Width, desc.Height, desc.MipLevels, desc.ArraySize, static_cast<unsigned>(desc.Format),
             static_cast<unsigned>(desc.Usage), desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED && g_device) {
        log_line("renderer: the Direct3D device was removed: %08lX", g_device->GetDeviceRemovedReason());
    }
}

void log_resource_failure(const char* what, HRESULT hr, const D3D11_BUFFER_DESC& desc) {
    static std::atomic<int> reported{0};
    if (reported++ >= 20) return;
    log_line("renderer: cannot create %s: error %08lX (%u bytes, usage %u, bind %X, cpu %X, misc %X)", what, hr,
             desc.ByteWidth, static_cast<unsigned>(desc.Usage), desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
}

// The surface at `address`, at least width x height. A smaller request reuses the existing target (the draw's
// viewport covers only its part); a larger one grows it and keeps what was drawn so far. Sizes are the game's;
// the texture is g_scale times larger.
Target* get_target(uint32_t address, uint32_t width, uint32_t height) {
    Target& target = g_targets[address];
    if (target.texture && target.width >= width && target.height >= height) return &target;
    width = std::max(width, target.width);
    height = std::max(height, target.height);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = scaled_size(width);
    desc.Height = scaled_size(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = target.format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target grown{};
    grown.format = target.format;
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &grown.texture); FAILED(hr)) {
        log_resource_failure("a larger render target", hr, desc);
        return nullptr;
    }
    g_device->CreateRenderTargetView(grown.texture.Get(), nullptr, &grown.rtv);
    g_device->CreateShaderResourceView(grown.texture.Get(), nullptr, &grown.srv);
    grown.width = width;
    grown.height = height;
    if (target.texture) {
        const D3D11_BOX box{0, 0, 0, scaled_size(target.width), scaled_size(target.height), 1};
        g_context->CopySubresourceRegion(grown.texture.Get(), 0, 0, 0, 0, target.texture.Get(), 0, &box);
    }
    target = grown;
    log_line("renderer: render target %ux%u at %08X", width, height, address);
    return &target;
}

ComPtr<ID3D11VertexShader> g_copy_vertex_shader;
ComPtr<ID3D11PixelShader> g_copy_pixel_shader;

// Switches a render target to a float format (keeping its contents), for the Xbox's signed blend equations: on
// UNORM a subtraction below zero would clamp and lose the count.
// What bind_draw_state last gave Direct3D: a draw like the one before (most are) sets only what differs. Code that
// binds other state (effects, occlusion, presenting) forgets it on the way out (ForgetBound).
struct BoundState {
    bool valid = false;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT viewport{};
    D3D11_RECT scissor{};
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11BlendState* blend = nullptr;
    float blend_color[4] = {};
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencil_reference = 0;
    CombinerConstants constants{};
    ID3D11ShaderResourceView* views[4] = {};
    ID3D11SamplerState* samplers[4] = {};
    ID3D11PixelShader* shader = nullptr;
    // What draw_vertex_data set after it (the vertex shader, its constants and vertex bytes, the index buffer).
    bool vertex_data = false;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11Buffer* vertex_constants = nullptr;
    ID3D11ShaderResourceView* vertex_view = nullptr;
    ID3D11Buffer* index_buffer = nullptr;
};
BoundState g_bound;
struct ForgetBound {
    ~ForgetBound() { g_bound.valid = false; }
};

void make_float_target(Target& target, uint32_t address) {
    const ForgetBound forget;
    if (target.format == DXGI_FORMAT_R16G16B16A16_FLOAT || !g_copy_pixel_shader) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = scaled_size(target.width);
    desc.Height = scaled_size(target.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target converted{};
    converted.format = desc.Format;
    converted.width = target.width;
    converted.height = target.height;
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &converted.texture); FAILED(hr)) {
        log_resource_failure("a converted render target", hr, desc);
        return;
    }
    g_device->CreateRenderTargetView(converted.texture.Get(), nullptr, &converted.rtv);
    g_device->CreateShaderResourceView(converted.texture.Get(), nullptr, &converted.srv);
    // Copy the contents with a full-surface triangle (CopyResource needs the same format).
    ID3D11RenderTargetView* rtv = converted.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(target.width), scaled_float(target.height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT all{0, 0, scaled(target.width), scaled(target.height)};
    g_context->RSSetScissorRects(1, &all);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_copy_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_copy_pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* source = target.srv.Get();
    g_context->PSSetShaderResources(0, 1, &source);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
    target = converted;
    log_line("renderer: render target at %08X switched to float (signed blending)", address);
}

// Depth buffers must match their render target's size exactly in D3D11: one per address and size.
DepthTarget* get_depth_target(uint32_t address, uint32_t width, uint32_t height) {
    g_zeta_addresses.insert(address);
    DepthTarget& target = g_depth_targets[(static_cast<uint64_t>(width) << 48) | (static_cast<uint64_t>(height) << 32) |
                                          address];
    if (target.texture && target.width == width && target.height == height) return &target;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = scaled_size(width);
    desc.Height = scaled_size(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;  // depth-stencil view and shader view of the same texture
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    target = DepthTarget{};
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &target.texture); FAILED(hr)) {
        log_resource_failure("a depth target", hr, desc);
        return nullptr;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_device->CreateDepthStencilView(target.texture.Get(), &dsv, &target.dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(target.texture.Get(), &srv, &target.srv);
    target.width = width;
    target.height = height;
    return &target;
}

// --- ambient occlusion ---
//
// Corners and contacts darkened from the depth buffer, applied to a frame's 3D scene before its HUD: at the first
// draw without depth test that follows depth-tested draws on a full-size target (once per depth clear, so each view
// of the split screen gets its own). The depth is not linear and the game's projection is unknown, but 1 - depth is
// proportional to 1/z, which varies linearly across the screen on a plane: comparing a pixel with the mean of two
// opposite neighbours gives zero on flat surfaces and a positive value in creases, relative to the pixel's own
// value whatever the scale. Large differences (an object in front of another) are left out, so outlines get no halo:
// left out of the average too, not counted as open. Counted as open, they lightened a shaded wall wherever its
// samples reached a figure in front of it: a pale box, the radius wide, around Midnight Club 3's pedestrians and
// lamp posts at night.

constexpr char kOcclusionPixelShader[] = R"(
cbuffer Occlusion : register(b0) { float radius; float strength; float2 unused; };
Texture2D<float> depth : register(t0);
float4 main(float4 position : SV_Position) : SV_Target {
    const int2 p = int2(position.xy);
    const float centre = 1 - depth.Load(int3(p, 0));  // proportional to 1/z
    if (centre <= 1e-5) return 1;                      // cleared to the far plane: sky
    // The same directions for every pixel: turned per pixel instead, they left a grain that showed through
    // anything bright drawn where the occlusion is strong (MC3's neon glow under the car).
    float occlusion = 0, counted = 0;
    [unroll] for (int i = 0; i < 12; ++i) {
        const float a = i * 0.2617994;                  // 15 degrees apart over a half turn
        const float r = radius * (0.35 + 0.65 * ((i * 5 + 3) % 12) / 11.0);
        const int2 o = int2(round(float2(cos(a), sin(a)) * r));
        const float one = 1 - depth.Load(int3(p + o, 0));
        const float two = 1 - depth.Load(int3(p - o, 0));
        const float crease = ((one + two) * 0.5 - centre) / centre;  // > 0: the pixel lies behind its neighbours
        const float weight = 1 - smoothstep(0.15, 0.4, crease);    // 0: across an outline
        occlusion += saturate(crease * 40) * weight;
        counted += weight;
    }
    // At least 3 of 12: a pixel nearly surrounded by outlines stays lighter rather than take one direction's value.
    const float light = 1 - strength * occlusion / max(counted, 3);
    return float4(light, light, light, 1);
}
)";

struct OcclusionConstants {
    float radius, strength, unused[2];
};

ComPtr<ID3D11PixelShader> g_occlusion_pixel_shader;
ComPtr<ID3D11Buffer> g_occlusion_constants;
ComPtr<ID3D11BlendState> g_multiply_blend;  // destination colour times the source (alpha untouched)
std::atomic<bool> g_effects_enabled{true};  // F10
std::atomic<RendererOverlay> g_overlay{nullptr};  // renderer_set_overlay
uint32_t g_self_read_surface = 0;  // the surface a draw last read while drawing into it (MC3's motion blur buffer)
std::unordered_set<uint32_t> g_shown_surfaces;  // surfaces renderer_present has shown (the frame buffers)

// Per depth buffer, since its last clear (the start of a scene; presents do not line up with scenes): 3D drawn,
// occlusion applied.
struct SceneState {
    bool drawn = false;
    bool occluded = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the rectangle its depth clear covered (a split-screen view), guest pixels
};
std::unordered_map<uint32_t, SceneState> g_scenes;
// Shown surfaces whose scene already has the effects (bloom, colour, vignette) from the end of the 3D scene, under the
// HUD: presenting them again (the same frame shown twice) must not add them a second time. Forgotten when the
// surface is cleared.
std::unordered_set<uint32_t> g_graded_surfaces;
uint32_t g_occlusion_scenes = 0, g_full_depth_clears = 0;  // for the log: every scene should get its pass

// The render thread (see "the render thread" below): draws and clears are queued by the thread that decodes the game's commands and
// done by another. What the decoding thread asks about the scene (renderer_over_shown_scene) is answered from what it
// has queued, not from what is drawn so far.
DWORD g_render_thread_id = 0;
std::unordered_map<uint32_t, bool> g_queued_scenes;     // by depth buffer: a scene draw queued since its last clear
std::unordered_map<uint32_t, uint32_t> g_queued_widths;  // by colour buffer: the widest surface asked for
void start_render_thread();
void flush_queue();  // waits until everything queued is drawn; nothing to do on the render thread itself

bool g_frame_scene = false;  // a draw in perspective since the last present (renderer_note_scene): not a film or loading screen
float g_shown_aspect = 0;    // the shape the last frame was shown in (renderer_aspect, 16:9 at most without 3D)
uint32_t g_picture_width = 0, g_picture_height = 0;  // the last frame shown
ShownPicture g_shown_picture{};                       // and where it is in the window
constexpr uint32_t kOverscan = 48;                    // 1 / kOverscan of each side is not shown

void apply_occlusion(Target& target, DepthTarget& depth) {
    const ForgetBound forget;
    const float strength = setting("ambient_occlusion");
    if (strength <= 0 || !g_occlusion_pixel_shader || !depth.srv || !g_effects_enabled) return;
    const OcclusionConstants constants{setting("ambient_occlusion_radius") * 14.0f * g_scale, std::min(strength, 1.0f)};
    g_context->UpdateSubresource(g_occlusion_constants.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11RenderTargetView* rtv = target.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);  // the depth is read, not bound
    // <GAME>_AO_VIEW=1: the occlusion alone, in grey, instead of the scene (to tune it).
    static const bool view_only = GetEnvironmentVariableA(XPORT_ENV("AO_VIEW"), nullptr, 0) != 0;
    g_context->OMSetBlendState(view_only ? nullptr : g_multiply_blend.Get(), nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(target.width), scaled_float(target.height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT all{0, 0, scaled(target.width), scaled(target.height)};
    g_context->RSSetScissorRects(1, &all);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_clear_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_occlusion_pixel_shader.Get(), nullptr, 0);
    ID3D11Buffer* buffer = g_occlusion_constants.Get();
    g_context->PSSetConstantBuffers(0, 1, &buffer);
    ID3D11ShaderResourceView* source = depth.srv.Get();
    g_context->PSSetShaderResources(0, 1, &source);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
}

// --- the depth buffer read as a colour texture ---
//
// Spider-Man 2 fades its stencil shadows with distance: a quad over the frame samples the depth buffer as an A8R8G8B8
// texture, and the next stage looks up a ramp with its alpha and red. The D24S8 word reads as A = depth bits 23..16,
// R = 15..8, G = 7..0, B = the stencil. The NV2A's depth stays on the GPU here, so the draw got whatever was in that
// memory: the shadow at full strength everywhere the stencil said shadow, as dark blocks on the sky and far walls.

constexpr char kDepthColourPixelShader[] = R"(
Texture2D<float> depth : register(t0);
float4 main(float4 position : SV_Position) : SV_Target {
    const uint d = (uint)round(depth.Load(int3(position.xy, 0)) * 16777215.0);
    return float4((d >> 8) & 255, d & 255, 0, d >> 16) / 255.0;  // shortcut: stencil (B) left 0, nothing reads it yet
}
)";
ComPtr<ID3D11PixelShader> g_depth_colour_pixel_shader;
std::unordered_map<uint32_t, Target> g_depth_colours;  // by depth buffer address, made again by each draw reading it

// The depth buffer at `address` (the largest when there are several sizes), or null.
DepthTarget* depth_target_at(uint32_t address) {
    DepthTarget* found = nullptr;
    for (auto& [key, depth] : g_depth_targets) {
        if (static_cast<uint32_t>(key) == address && depth.srv && (!found || depth.width > found->width)) found = &depth;
    }
    return found;
}

// For a draw that samples a depth buffer: its contents now, as colour, for texture_view.
void refresh_depth_colour(uint32_t address) {
    DepthTarget* depth = depth_target_at(address);
    if (!depth || !g_depth_colour_pixel_shader) return;
    const ForgetBound forget;
    Target& colour = g_depth_colours[address];
    if (!colour.texture || colour.width != depth->width || colour.height != depth->height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = scaled_size(depth->width);
        desc.Height = scaled_size(depth->height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        colour = Target{};
        if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &colour.texture); FAILED(hr)) {
            log_resource_failure("a depth buffer read as colour", hr, desc);
            return;
        }
        g_device->CreateRenderTargetView(colour.texture.Get(), nullptr, &colour.rtv);
        g_device->CreateShaderResourceView(colour.texture.Get(), nullptr, &colour.srv);
        colour.width = depth->width;
        colour.height = depth->height;
        colour.format = desc.Format;
        log_line("renderer: depth buffer at %08X (%ux%u) read as a texture", address, depth->width, depth->height);
    }
    ID3D11RenderTargetView* rtv = colour.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);  // unbinds the depth buffer, which is read
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(colour.width), scaled_float(colour.height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT all{0, 0, scaled(colour.width), scaled(colour.height)};
    g_context->RSSetScissorRects(1, &all);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_clear_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_depth_colour_pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* source = depth->srv.Get();
    g_context->PSSetShaderResources(0, 1, &source);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
}

// The other way: colour drawn into memory that a later draw uses as its depth buffer. Midnight Club 3 copies its
// scene's depth (read as colour, above) into a half-size surface, then tests its headlights' light volumes against
// that memory as a depth buffer. Here the two are different textures, so the light volumes were tested against an
// empty depth buffer and lit nothing. The colour's A, R and G are the depth's three bytes (D24S8, high byte first).
constexpr char kColourToDepthPixelShader[] = R"(
Texture2D colour : register(t0);
float main(float4 position : SV_Position) : SV_Depth {
    const float4 c = round(colour.Load(int3(position.xy, 0)) * 255);
    return (c.a * 65536 + c.r * 256 + c.g) / 16777215;  // shortcut: B, the stencil, is left out; nothing reads it yet
}
)";
ComPtr<ID3D11PixelShader> g_colour_to_depth_pixel_shader;
ComPtr<ID3D11DepthStencilState> g_depth_overwrite;

void colour_to_depth(Target& colour, DepthTarget& depth, uint32_t address) {
    if (!g_colour_to_depth_pixel_shader || colour.width != depth.width || colour.height != depth.height) return;
    const ForgetBound forget;
    if (!g_depth_overwrite) {
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        g_device->CreateDepthStencilState(&desc, &g_depth_overwrite);
        log_line("renderer: colour drawn at %08X used as a depth buffer (%ux%u)", address, depth.width, depth.height);
    }
    ID3D11ShaderResourceView* none[4] = {};
    g_context->PSSetShaderResources(0, 4, none);  // the colour may still be bound as a texture
    g_context->OMSetRenderTargets(0, nullptr, depth.dsv.Get());
    g_context->OMSetDepthStencilState(g_depth_overwrite.Get(), 0);
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(depth.width), scaled_float(depth.height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT all{0, 0, scaled(depth.width), scaled(depth.height)};
    g_context->RSSetScissorRects(1, &all);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_clear_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_colour_to_depth_pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* source = colour.srv.Get();
    g_context->PSSetShaderResources(0, 1, &source);
    g_context->Draw(3, 0);
    g_context->PSSetShaderResources(0, 1, none);
}

D3D11_BLEND blend_factor(uint32_t gl) {
    switch (gl) {
        case 0x0000: return D3D11_BLEND_ZERO;
        case 0x0001: return D3D11_BLEND_ONE;
        case 0x0300: return D3D11_BLEND_SRC_COLOR;
        case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
        case 0x0302: return D3D11_BLEND_SRC_ALPHA;
        case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x0304: return D3D11_BLEND_DEST_ALPHA;
        case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
        case 0x0306: return D3D11_BLEND_DEST_COLOR;
        case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
        case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
        default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND alpha_blend_factor(uint32_t gl) {
    switch (gl) {  // colour factors are not allowed for alpha
        case 0x0300: return D3D11_BLEND_SRC_ALPHA;
        case 0x0301: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x0306: return D3D11_BLEND_DEST_ALPHA;
        case 0x0307: return D3D11_BLEND_INV_DEST_ALPHA;
        default: return blend_factor(gl);
    }
}

D3D11_BLEND_OP blend_operation(uint32_t gl) {
    switch (gl) {
        case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
        // Xbox "signed" equations (D3DBLENDOP_REVSUBTRACTSIGNED / ADDSIGNED): see signed_source below; the 1.0 they
        // are sent counts as -1/255, so subtracting it adds. Drawn into a float target, which does not clamp.
        case 0xF005: return D3D11_BLEND_OP_ADD;
        case 0xF006: return D3D11_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D11_BLEND_OP_MIN;
        case 0x8008: return D3D11_BLEND_OP_MAX;
        default: return D3D11_BLEND_OP_ADD;
    }
}

// The signed equations read the source as a two's complement byte (hardware tests, xemu issue 996): the 1.0 (0xFF)
// that Midnight Club 3 sends to count its headlights' light volumes in alpha is -1/255, one step per face, so that
// sixteen nested volumes make a soft pool of light. Taken as 1.0 the count saturated: no light, or solid white.
// shortcut: weighted by a constant 1/255 with the operation swapped, exact for a source of 1.0 only (a source below
// 0.5 counts positive on the Xbox); a game sending other values needs the conversion in the pixel shader.
bool signed_source(const DrawState& state) {
    return state.blend_enable && (state.blend_equation == 0xF005 || state.blend_equation == 0xF006) &&
           state.blend_source == 1;
}

ID3D11BlendState* blend_state(const DrawState& state) {
    const auto key = std::make_tuple(state.blend_enable, state.blend_source, state.blend_destination,
                                     state.blend_equation, state.color_mask);
    ComPtr<ID3D11BlendState>& cached = g_blend_states[key];
    if (cached) return cached.Get();
    D3D11_BLEND_DESC desc{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
    rt.BlendEnable = state.blend_enable;
    rt.SrcBlend = blend_factor(state.blend_source);
    rt.DestBlend = blend_factor(state.blend_destination);
    rt.BlendOp = blend_operation(state.blend_equation);
    rt.SrcBlendAlpha = alpha_blend_factor(state.blend_source);
    rt.DestBlendAlpha = alpha_blend_factor(state.blend_destination);
    if (signed_source(state)) rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;  // 1/255 (bind_draw_state)
    rt.BlendOpAlpha = rt.BlendOp;
    rt.RenderTargetWriteMask = static_cast<UINT8>(((state.color_mask & 0x10000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
                                                  ((state.color_mask & 0x100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
                                                  ((state.color_mask & 0x1) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
                                                  ((state.color_mask & 0x1000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    g_device->CreateBlendState(&desc, &cached);
    return cached.Get();
}

D3D11_STENCIL_OP stencil_operation(uint32_t gl) {
    switch (gl) {
        case 0x0000: return D3D11_STENCIL_OP_ZERO;
        case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D11_STENCIL_OP_INVERT;
        case 0x8507: return D3D11_STENCIL_OP_INCR;
        case 0x8508: return D3D11_STENCIL_OP_DECR;
        default: return D3D11_STENCIL_OP_KEEP;  // 0x1E00
    }
}

ID3D11DepthStencilState* depth_state(const DrawState& state) {
    const auto key = std::make_tuple(state.depth_test, state.depth_function, state.depth_write, state.stencil_test,
                                     state.stencil_function, state.stencil_read_mask, state.stencil_write_mask,
                                     state.stencil_fail, state.stencil_depth_fail, state.stencil_pass);
    ComPtr<ID3D11DepthStencilState>& cached = g_depth_states[key];
    if (cached) return cached.Get();
    D3D11_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = state.depth_test || state.depth_write;
    desc.DepthWriteMask = state.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = state.depth_test ? static_cast<D3D11_COMPARISON_FUNC>((state.depth_function & 7) + 1)
                                      : D3D11_COMPARISON_ALWAYS;
    desc.StencilEnable = state.stencil_test;
    desc.StencilReadMask = static_cast<UINT8>(state.stencil_read_mask);
    desc.StencilWriteMask = static_cast<UINT8>(state.stencil_write_mask);
    D3D11_DEPTH_STENCILOP_DESC face{};
    face.StencilFailOp = stencil_operation(state.stencil_fail);
    face.StencilDepthFailOp = stencil_operation(state.stencil_depth_fail);
    face.StencilPassOp = stencil_operation(state.stencil_pass);
    face.StencilFunc = static_cast<D3D11_COMPARISON_FUNC>((state.stencil_function & 7) + 1);
    desc.FrontFace = desc.BackFace = face;
    g_device->CreateDepthStencilState(&desc, &cached);
    return cached.Get();
}

D3D11_TEXTURE_ADDRESS_MODE address_mode(uint32_t nv) {
    switch (nv) {
        case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

ID3D11SamplerState* sampler(const TextureStage& stage) {
    ComPtr<ID3D11SamplerState>& cached = g_samplers[{stage.address_mode, stage.filter}];
    if (cached) return cached.Get();
    D3D11_SAMPLER_DESC desc{};
    // NV097_SET_TEXTURE_FILTER: magnification 1 nearest, 2 linear; minification 1 nearest, 2 linear, 3-6 the
    // four mipmap combinations (nearest/linear within a level, nearest/linear between levels), 7 convolution.
    const uint32_t min = (stage.filter >> 16) & 0xFF;
    const bool linear_mag = ((stage.filter >> 24) & 0xF) != 1;
    const bool linear_min = min == 2 || min == 4 || min == 6 || min == 7;
    const bool mipmaps = min >= 3 && min <= 6;
    const bool linear_mip = min == 5 || min == 6;
    static constexpr D3D11_FILTER kFilters[8] = {
        D3D11_FILTER_MIN_MAG_MIP_POINT,          D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR,
        D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR,
        D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT,   D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,   D3D11_FILTER_MIN_MAG_MIP_LINEAR};
    desc.Filter = kFilters[(linear_min ? 4 : 0) | (linear_mag ? 2 : 0) | (linear_mip ? 1 : 0)];
    // Smooth (trilinear) filtering becomes anisotropic: textures seen at a slant (roads, walls) stay sharp
    // (setting anisotropy, 1..16, 1 = off).
    static const UINT anisotropy = std::clamp<UINT>(static_cast<UINT>(setting_at_start("anisotropy")), 1, 16);
    if (linear_min && linear_mag && mipmaps && anisotropy > 1) {
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = anisotropy;
    }
    desc.AddressU = address_mode(stage.address_mode & 0xF);
    desc.AddressV = address_mode((stage.address_mode >> 8) & 0xF);
    desc.AddressW = address_mode((stage.address_mode >> 16) & 0xF);
    desc.MaxLOD = mipmaps ? D3D11_FLOAT32_MAX : 0;  // without mipmap filtering only the top level is used
    // The filter's LOD bias, bits 0-12: signed, 8 fractional bits. Spider-Man 2 asks for 0x1F01 (-255/256, about one
    // level sharper) on many textures; without it they came out a level too blurred.
    if (mipmaps) {
        const int32_t raw = static_cast<int32_t>(stage.filter & 0x1FFF);
        desc.MipLODBias = static_cast<float>(raw >= 0x1000 ? raw - 0x2000 : raw) / 256.0f;
    }
    // Debugging the textures: <GAME>_TEX_MAXLOD=<n> reads no level above n, <GAME>_TEX_LODBIAS=<f> replaces the bias,
    // <GAME>_TEX_LOG=1 logs each filter and address mode once.
    static const char* max_lod = std::getenv(XPORT_ENV("TEX_MAXLOD"));
    static const char* lod_bias = std::getenv(XPORT_ENV("TEX_LODBIAS"));
    static const bool log_samplers = std::getenv(XPORT_ENV("TEX_LOG")) != nullptr;
    if (max_lod) desc.MaxLOD = std::min(desc.MaxLOD, static_cast<float>(std::atof(max_lod)));
    if (lod_bias) desc.MipLODBias = static_cast<float>(std::atof(lod_bias));
    if (log_samplers) {
        log_line("renderer: sampler filter %08X (bias bits %04X, min %u, mag %u) address %08X", stage.filter, stage.filter & 0x1FFF,
                 min, (stage.filter >> 24) & 0xF, stage.address_mode);
    }
    g_device->CreateSamplerState(&desc, &cached);
    return cached.Get();
}

// Current write-tracking generation, refreshed at most every 2 ms: a write that lands just after a refresh is
// picked up by the next one.
uint32_t g_write_generation = 0;
uint64_t g_write_refreshed_at = 0;

void refresh_write_tracking() {
    static const uint64_t interval = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart) / 500;
    }();
    const uint64_t now = ticks();
    if (g_write_generation && now - g_write_refreshed_at < interval) return;
    g_write_generation = refresh_physical_writes();
    g_write_refreshed_at = now;
}

// textures_replace\<hash>.png (or .jpg) in the data folder: a picture drawn instead of the game's texture whose
// bytes have that hash (16 hex digits, as <GAME>_DUMP_2D and <GAME>_DUMP_TEXTURE name their files), of any size. For
// pictures made from one's own copy of the game, sharper or redrawn; <port> ships none. Linear textures (addressed
// in texels) are left as they are. For a texture with a palette, a PNG with a palette of its own (8 bits a pixel) is
// taken as indices into the game's palette: the picture is larger, the colours stay the game's, as it changes them (a
// HUD the player colours). <GAME>_DUMP_2D saves such a texture's indices and palette beside its picture.
std::unordered_map<uint64_t, std::filesystem::path> g_replacements;

void find_replacements() {
    const std::filesystem::path folder = data_directory() / "textures_replace";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::wstring extension = entry.path().extension().wstring();
        if (extension != L".png" && extension != L".jpg" && extension != L".PNG" && extension != L".JPG") continue;
        const std::wstring stem = entry.path().stem().wstring();
        if (stem.size() != 16) continue;
        wchar_t* end = nullptr;
        const uint64_t hash = std::wcstoull(stem.c_str(), &end, 16);
        if (end == stem.c_str() + 16) g_replacements[hash] = entry.path();
    }
    if (!g_replacements.empty()) log_line("renderer: %zu replacement textures", g_replacements.size());
}

// The picture `file` as BGRA, through Windows' image decoders (PNG, JPEG). With `palette` (256 BGRA entries) a picture
// of 8 bits a pixel gives its indices, coloured from that palette instead of its own.
bool load_picture(const std::filesystem::path& file, std::vector<uint8_t>& bgra, UINT& width, UINT& height,
                  const uint8_t* palette = nullptr) {
    thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // the launcher's thread and the GPU's
    (void)com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }
    WICPixelFormatGUID format{};
    if (palette && SUCCEEDED(frame->GetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat8bppIndexed)) {
        if (FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 8192 || height > 8192) return false;
        std::vector<uint8_t> indices(static_cast<size_t>(width) * height);
        if (FAILED(frame->CopyPixels(nullptr, width, static_cast<UINT>(indices.size()), indices.data()))) return false;
        bgra.resize(indices.size() * 4);
        for (size_t i = 0; i < indices.size(); ++i) std::memcpy(&bgra[i * 4], palette + indices[i] * 4, 4);
        return true;
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || !width || !height || width > 8192 || height > 8192) {
        return false;
    }
    bgra.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bgra.size()), bgra.data()));
}

// A replacement picture as a texture with its mip levels made by the graphics card.
bool make_replacement(const std::filesystem::path& file, CachedTexture& cached, const uint8_t* palette) {
    std::vector<uint8_t> bgra;
    UINT width = 0, height = 0;
    if (!load_picture(file, bgra, width, height, palette)) {
        log_line("renderer: cannot read the replacement texture %s", file.filename().string().c_str());
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 0;  // all of them
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &texture)) ||
        FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
        log_resource_failure("a replacement texture", E_FAIL, desc);
        return false;
    }
    g_context->UpdateSubresource(texture.Get(), 0, nullptr, bgra.data(), width * 4, 0);
    g_context->GenerateMips(srv.Get());
    cached.texture = texture;
    cached.srv = srv;
    return true;
}

// <GAME>_DUMP_TEXTURE=all: every texture the game draws with, once, as textures_dump\<hash>.png in the data folder
// (BGRA with its alpha, whatever the game's format): the pictures to redraw or enlarge with a tool of one's own, and
// to put back in textures_replace under the same name.
void dump_for_replacement(const TextureImage& image, uint64_t hash) {
    static std::unordered_set<uint64_t> dumped;
    if (!dumped.insert(hash).second) return;
    const std::filesystem::path folder = data_directory() / "textures_dump";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    wchar_t name[32];
    std::swprintf(name, 32, L"%016llX.png", static_cast<unsigned long long>(hash));
    const std::filesystem::path file = folder / name;
    if (std::filesystem::exists(file, error)) return;
    const TextureImage bgra = texture_bgra(image);
    if (bgra.layout != TextureLayout::Bgra8) return;
    thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool saved =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(bgra.width, bgra.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(frame->WritePixels(bgra.height, bgra.row_pitch, static_cast<UINT>(bgra.data.size()),
                                     const_cast<BYTE*>(bgra.data.data()))) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (!saved) {
        stream.Reset();
        std::filesystem::remove(file, error);  // no half-written picture left to be taken for a replacement's source
        log_line("renderer: could not save %s", file.string().c_str());
    }
}

// `flat`: the draw is flat over the 3D scene (the HUD): with sharp_2d its textures are enlarged (texture_upscale.cpp),
// and those the game names (renderer_game_enlarge); with sharp_textures those of the scene as well. Not the linear
// ones, which are addressed in texels.
ID3D11ShaderResourceView* find_texture_view(const TextureStage& stage, bool flat, ViewMemo& memo) {
    const TextureDescriptor& d = stage.descriptor;
    static const bool sharp_2d = setting_at_start("sharp_2d") != 0;
    static const bool sharp_textures = setting_at_start("sharp_textures") != 0;
    const bool enlarge_2d = (flat || renderer_game_enlarge(d)) && sharp_2d;
    const bool enlarge = (enlarge_2d || sharp_textures) && !texture_is_linear(d);
    // A texture that is a render target (render to texture) is sampled from the GPU copy.
    const auto target = g_targets.find(d.address);
    if (target != g_targets.end()) return target->second.srv.Get();
    if (const auto depth = g_depth_colours.find(d.address); depth != g_depth_colours.end()) return depth->second.srv.Get();

    uint32_t width = 0, height = 0, bytes = 0;
    if (!texture_extent(d, &width, &height, &bytes)) return nullptr;
    // The game's own mip levels follow the top one; they are checked and hashed with it.
    uint32_t levels = texture_levels(d);
    if (levels > 1) {
        const uint32_t all = texture_total_bytes(d);
        if (physical_pointer(d.address + all - 1)) {
            bytes = all;
        } else {
            levels = 1;
        }
    }
    const uint8_t* memory = physical_pointer(d.address);
    if (!memory || !physical_pointer(d.address + bytes - 1)) return nullptr;
    // The palette only where the format reads it: for the others its address changes with the draws before, and a
    // new cache entry each time made the cache grow without end.
    const uint32_t palette = texture_is_paletted(d) ? d.palette : 0;
    const TextureKey key = std::make_tuple(d.address, d.format, d.pitch, d.rect, palette, enlarge);
    CachedTexture& cached = g_textures[key];
    memo.cached = &cached, memo.bytes = bytes, memo.palette = palette;
    // On the way out: the entry may be rebuilt (cached = CachedTexture{}) on the way.
    struct Touch {
        CachedTexture& cached;
        ~Touch() { cached.last_used = g_texture_clock; }
    } touch{cached};
    std::vector<TextureImage> larger;
    if (cached.waiting && g_enlarger.take(key, cached.content_hash, larger)) {
        cached.waiting = false;
        uint64_t larger_bytes = 0;
        for (const TextureImage& level : larger) larger_bytes += level.data.size();
        // Past the budget the scene's textures stay as the game's; the HUD's few are always enlarged.
        if (!enlarge_2d && g_enlarged_bytes + larger_bytes > kEnlargedBudget) {
            larger.clear();
            static bool logged = false;
            if (!logged) log_line("renderer: %llu MB of enlarged textures, the rest stay as the game's",
                                  static_cast<unsigned long long>(g_enlarged_bytes >> 20));
            logged = true;
        }
        if (!larger.empty()) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = larger[0].width;
            desc.Height = larger[0].height;
            desc.MipLevels = static_cast<UINT>(larger.size());
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            std::vector<D3D11_SUBRESOURCE_DATA> data;
            for (const TextureImage& level : larger) data.push_back({level.data.data(), level.row_pitch, 0});
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> srv;
            if (SUCCEEDED(g_device->CreateTexture2D(&desc, data.data(), &texture)) &&
                SUCCEEDED(g_device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
                cached.texture = texture;
                cached.srv = srv;
                cached.enlarged.set(static_cast<uint32_t>(larger_bytes));
            } else {
                log_resource_failure("an enlarged texture", E_FAIL, desc);
            }
        }
    }
    // Unchanged memory needs no look at all; written memory is hashed, and decoded again only if it differs.
    uint32_t written = physical_write_generation(d.address, bytes);
    if (palette) written = std::max(written, physical_write_generation(palette, 1024));
    static const bool always_hash = GetEnvironmentVariableA(XPORT_ENV("ALWAYS_HASH"), nullptr, 0) != 0;  // debugging: never trust write tracking
    if (!always_hash && cached.srv && written <= cached.checked_generation) return cached.srv.Get();

    const uint64_t hash_start = ticks();
    // The texture's own bytes name its replacement (the palette address is set for every format, and its memory
    // changes between runs).
    const auto hash_piece = [](const uint8_t* data, uint32_t size) {
        g_stats.hashed_bytes += size;
        return change_hash(data, size);
    };
    const uint64_t hash = hash_written_pages(cached.page_hashes, memory, d.address & 0x7FFFFFFF, bytes,
                                             always_hash ? 0 : cached.checked_generation,
                                             [](uint32_t address) { return physical_write_generation(address, 1); }, hash_piece) ^
                          (palette ? hash_piece(physical_pointer(palette), 1024) : 0);
    g_stats.hash_ticks += ticks() - hash_start;
    cached.checked_generation = g_write_generation;
    if (cached.srv && cached.content_hash == hash) return cached.srv.Get();
    static const bool replacements = (find_replacements(), !g_replacements.empty());
    // The name of replacement and dumped pictures: hash_memory of the texture's own bytes, as in earlier versions.
    static const bool dumping = GetEnvironmentVariableA(XPORT_ENV("DUMP_TEXTURE"), nullptr, 0) != 0 ||
                                GetEnvironmentVariableA(XPORT_ENV("DUMP_2D"), nullptr, 0) != 0;
    const uint64_t own_hash = replacements || dumping ? hash_memory(memory, bytes) : 0;
    if (replacements && !texture_is_linear(d)) {
        const auto replacement = g_replacements.find(own_hash);
        if (replacement != g_replacements.end()) {
            cached = CachedTexture{};
            if (make_replacement(replacement->second, cached, palette ? physical_pointer(palette) : nullptr)) {
                cached.content_hash = hash;
                log_line("renderer: texture %08X replaced by %s", d.address,
                         replacement->second.filename().string().c_str());
                return cached.srv.Get();
            }
        }
    }

    const uint64_t upload_start = ticks();
    ProfileScope upload_scope(ProfileTime::texture_upload);
    struct Account {
        uint64_t start;
        ~Account() {
            g_stats.upload_ticks += ticks() - start;
            ++g_stats.uploads;
        }
    } account{upload_start};
    TextureImage image = decode_texture(d);
    if (image.layout == TextureLayout::Unsupported) return nullptr;
    // A game may draw over a texture as it is decoded (renderer_game_texture); then its lower levels, which keep the
    // original, are left out.
    const bool game_drew = renderer_game_texture(d, memory, bytes, image);
    // <GAME>_DUMP_TEXTURE=<hex address>: save the textures in that 4 KB page once decoded (BGRA with alpha) as
    // texture-<address>-<hash>.bmp (resources move a little between runs; the hash names a replacement).
    static const bool dump_all = [] {
        char value[16] = {};
        return GetEnvironmentVariableA(XPORT_ENV("DUMP_TEXTURE"), value, sizeof(value)) && std::strcmp(value, "all") == 0;
    }();
    if (dump_all && !texture_is_linear(d) && !game_drew) dump_for_replacement(image, own_hash);
    static const uint32_t dump_texture = [] {
        char value[16] = {};
        return !dump_all && GetEnvironmentVariableA(XPORT_ENV("DUMP_TEXTURE"), value, sizeof(value))
                   ? std::strtoul(value, nullptr, 16)
                   : 0u;
    }();
    // <GAME>_DUMP_2D=1: also every texture of the HUD's flat draws (those sharp_2d enlarges). Block-compressed ones as
    // they are, in a DDS file (texture-<address>-<hash>.dds).
    static const bool dump_2d = GetEnvironmentVariableA(XPORT_ENV("DUMP_2D"), nullptr, 0) != 0;
    const bool dump = (dump_texture && (d.address & ~0xFFFu) == (dump_texture & ~0xFFFu)) || (dump_2d && flat);
    if (dump && image.layout != TextureLayout::Bgra8) {
        char name[64];
        std::snprintf(name, sizeof(name), "texture-%08X-%016llX.dds", d.address,
                      static_cast<unsigned long long>(own_hash));
        if (FILE* file = std::fopen(name, "wb")) {
            uint32_t header[32] = {0x20534444, 124, 0x1007, image.height, image.width, image.row_pitch, 0, 1};
            header[19] = 32;      // pixel format size
            header[20] = 4;       // DDPF_FOURCC
            header[21] = image.layout == TextureLayout::Bc1   ? 0x31545844   // "DXT1"
                         : image.layout == TextureLayout::Bc2 ? 0x33545844   // "DXT3"
                                                              : 0x35545844;  // "DXT5"
            header[27] = 0x1000;  // DDSCAPS_TEXTURE
            std::fwrite(header, sizeof(header), 1, file);
            std::fwrite(image.data.data(), 1, image.data.size(), file);
            std::fclose(file);
        }
    }
    if (dump && image.layout == TextureLayout::Bgra8) {
        char name[64];
        std::snprintf(name, sizeof(name), "texture-%08X-%016llX.bmp", d.address,
                      static_cast<unsigned long long>(own_hash));
        if (FILE* file = std::fopen(name, "wb")) {
            BITMAPFILEHEADER header{};
            BITMAPINFOHEADER info{};
            header.bfType = 0x4D42;
            header.bfOffBits = sizeof(header) + sizeof(info);
            header.bfSize = header.bfOffBits + image.width * 4 * image.height;
            info.biSize = sizeof(info);
            info.biWidth = static_cast<LONG>(image.width);
            info.biHeight = -static_cast<LONG>(image.height);
            info.biPlanes = 1;
            info.biBitCount = 32;
            std::fwrite(&header, sizeof(header), 1, file);
            std::fwrite(&info, sizeof(info), 1, file);
            std::fwrite(image.data.data(), 1, image.data.size(), file);
            std::fclose(file);
        }
    }
    if (dump && palette && physical_pointer(palette)) {  // for a replacement by indices: the texture's bytes as they
                                                         // are (swizzled indices), then the 256 BGRA entries
        char name[64];
        std::snprintf(name, sizeof(name), "texture-%08X-%016llX.idx", d.address,
                      static_cast<unsigned long long>(own_hash));
        if (FILE* file = std::fopen(name, "wb")) {
            std::fwrite(memory, 1, bytes, file);
            std::fwrite(physical_pointer(palette), 1, 1024, file);
            std::fclose(file);
        }
    }
    std::vector<TextureImage> lower_levels;
    for (uint32_t level = 1; level < levels && !game_drew; ++level) {
        lower_levels.push_back(decode_texture(texture_level(d, level)));
        if (lower_levels.back().layout != image.layout) {
            lower_levels.clear();
            break;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = 1 + static_cast<UINT>(lower_levels.size());
    desc.ArraySize = 1;
    desc.Format = image.layout == TextureLayout::Bc1   ? DXGI_FORMAT_BC1_UNORM
                  : image.layout == TextureLayout::Bc2 ? DXGI_FORMAT_BC2_UNORM
                  : image.layout == TextureLayout::Bc3 ? DXGI_FORMAT_BC3_UNORM
                                                       : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> data{{image.data.data(), image.row_pitch, 0}};
    for (const TextureImage& level : lower_levels) data.push_back({level.data.data(), level.row_pitch, 0});
    cached = CachedTexture{};
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, data.data(), &cached.texture); FAILED(hr)) {
        log_resource_failure("a texture", hr, desc);
        return nullptr;
    }
    g_device->CreateShaderResourceView(cached.texture.Get(), nullptr, &cached.srv);
    cached.content_hash = hash;
    if (enlarge && (enlarge_2d || g_enlarged_bytes < kEnlargedBudget)) {
        cached.waiting = true;
        g_enlarger.request(key, hash, image, !enlarge_2d, lower_levels);
    }
    return cached.srv.Get();
}

ID3D11ShaderResourceView* texture_view(const TextureStage& stage, bool flat) {
    static const bool always_hash = GetEnvironmentVariableA(XPORT_ENV("ALWAYS_HASH"), nullptr, 0) != 0;
    const TextureDescriptor& d = stage.descriptor;
    ViewMemo& memo = g_view_memo[((d.address >> 4) * 0x9E3779B1u ^ d.format ^ (flat ? 1u : 0u)) >> 23];
    if (memo.cached && memo.flat == flat && memo.targets == g_targets.size() &&
        memo.depth_colours == g_depth_colours.size() && !memo.cached->waiting && memo.cached->srv &&
        memo.descriptor.address == d.address && memo.descriptor.format == d.format &&
        memo.descriptor.pitch == d.pitch && memo.descriptor.rect == d.rect &&
        (!memo.palette || memo.palette == d.palette)) {  // the palette only where the format reads it
        bool unwritten = memo.generation == g_write_generation;
        if (!unwritten) {
            uint32_t written = physical_write_generation(d.address, memo.bytes);
            if (memo.palette) written = std::max(written, physical_write_generation(memo.palette, 1024));
            unwritten = written <= memo.cached->checked_generation;
        }
        if (unwritten) {
            memo.generation = g_write_generation;
            memo.cached->last_used = g_texture_clock;
            return memo.cached->srv.Get();
        }
    }
    memo = ViewMemo{};
    ID3D11ShaderResourceView* view = find_texture_view(stage, flat, memo);
    const bool keep = view && memo.cached && view == memo.cached->srv.Get() && !always_hash;
    if (!keep) memo.cached = nullptr;
    memo.descriptor = d, memo.flat = flat, memo.generation = g_write_generation;
    memo.targets = g_targets.size(), memo.depth_colours = g_depth_colours.size();
    return view;
}

// Named after the shader's text: any change of it is a new file.
std::filesystem::path shader_cache_file(const char* prefix, const std::string& text) {
    uint64_t hash = 14695981039346656037ull;
    for (const char ch : text) hash = (hash ^ static_cast<uint8_t>(ch)) * 1099511628211ull;
    char file_name[40];
    std::snprintf(file_name, sizeof(file_name), "%s-%016llX.cso", prefix, static_cast<unsigned long long>(hash));
    return data_directory() / "shader_cache" / file_name;
}

// Compiled shaders are kept in the data folder (those this PC compiled) and may ship in the program's folder (those
// the port compiled while it was tested), so that even the first run compiles nothing while playing.
std::filesystem::path shipped_shader_file(const std::filesystem::path& file) {
    return program_directory() / "shader_cache" / file.filename();
}

std::vector<char> read_cached_shader(const std::filesystem::path& file) {
    for (const std::filesystem::path& path : {file, shipped_shader_file(file)}) {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!code.empty()) return code;
    }
    return {};
}

// Every cached shader created on a helper thread at start, by file name: the first draw that needs one finds it ready
// instead of reading it and handing it to the driver on the GPU thread, a hitch the first time each effect shows.
std::mutex g_preload_mutex;
std::unordered_map<std::string, ComPtr<ID3D11PixelShader>> g_preloaded_pixel;
std::unordered_map<std::string, ComPtr<ID3D11VertexShader>> g_preloaded_vertex;

template <typename Shader>
ComPtr<Shader> take_preloaded(std::unordered_map<std::string, ComPtr<Shader>>& shaders, const std::filesystem::path& file) {
    std::lock_guard<std::mutex> lock(g_preload_mutex);
    const auto found = shaders.find(file.filename().string());
    return found != shaders.end() ? found->second : nullptr;
}

void preload_shaders() {
    std::thread([] {
        const uint64_t start = ticks();
        size_t count = 0;
        for (const std::filesystem::path& folder :
             {data_directory() / "shader_cache", program_directory() / "shader_cache"}) {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
                const std::string name = entry.path().filename().string();
                const bool pixel = name.rfind("ps-", 0) == 0, vertex = name.rfind("vp-", 0) == 0;
                if ((!pixel && !vertex) || entry.path().extension() != ".cso") continue;
                {
                    std::lock_guard<std::mutex> lock(g_preload_mutex);
                    if (g_preloaded_pixel.count(name) || g_preloaded_vertex.count(name)) continue;  // data folder first
                }
                std::ifstream in(entry.path(), std::ios::binary);
                const std::vector<char> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (code.empty()) continue;
                ComPtr<ID3D11PixelShader> pixel_shader;
                ComPtr<ID3D11VertexShader> vertex_shader;
                const bool made = pixel ? SUCCEEDED(g_device->CreatePixelShader(code.data(), code.size(), nullptr, &pixel_shader))
                                        : SUCCEEDED(g_device->CreateVertexShader(code.data(), code.size(), nullptr, &vertex_shader));
                if (!made) continue;
                std::lock_guard<std::mutex> lock(g_preload_mutex);
                if (pixel) g_preloaded_pixel[name] = pixel_shader;
                else g_preloaded_vertex[name] = vertex_shader;
                ++count;
            }
        }
        log_line("renderer: %zu cached shaders ready in %.0f ms", count, milliseconds(ticks() - start));
    }).detach();
}

// A combiner shader not compiled yet (the first time a place is seen; afterwards shader_cache has it) is compiled on a
// helper thread, two at a time. A draw that needs it waits while this frame's waits stay within kCompileWaitMs, and
// past that is left out until the shader is ready: a few draws a frame or two late, instead of a pause (6 ms a shader
// on average, 33 at most, 35 to 70 of them on entering a city). Set by renderer_present: g_compile_waited_ms.
constexpr double kCompileWaitMs = 8;
double g_compile_waited_ms = 0;  // in the current frame, GPU thread
std::unordered_map<uint64_t, std::shared_future<ComPtr<ID3D11PixelShader>>> g_pixel_pending;
std::mutex g_compile_mutex;
std::condition_variable g_compile_free;
int g_compiling = 0;

ComPtr<ID3D11PixelShader> compile_pixel_shader(const std::string& source, const std::filesystem::path& file) {
    {
        std::unique_lock<std::mutex> lock(g_compile_mutex);
        g_compile_free.wait(lock, [] { return g_compiling < 2; });
        ++g_compiling;
    }
    ComPtr<ID3D11PixelShader> shader;
    const uint64_t start = ticks();
    if (ComPtr<ID3DBlob> code = compile(source.c_str(), source.size(), "ps_5_0", "combiner")) {
        if (SUCCEEDED(g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) {
            std::error_code error;
            std::filesystem::create_directories(file.parent_path(), error);
            std::ofstream(file, std::ios::binary)
                .write(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
        }
        log_line("renderer: combiner shader %s compiled in %.1f ms", file.filename().string().c_str(),
                 milliseconds(ticks() - start));
    } else {
        log_line("%s", source.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(g_compile_mutex);
        --g_compiling;
    }
    g_compile_free.notify_one();
    return shader;
}

ID3D11PixelShader* pixel_shader(const CombinerProgram& program) {
    const uint64_t key = combiner_hash(program);
    const auto it = g_pixel_shaders.find(key);
    if (it != g_pixel_shaders.end()) return it->second.Get();
    auto pending = g_pixel_pending.find(key);
    if (pending == g_pixel_pending.end()) {
        std::string source = generate_pixel_shader(program);
        std::filesystem::path file = shader_cache_file("ps", source);
        ComPtr<ID3D11PixelShader> shader = take_preloaded(g_preloaded_pixel, file);
        const std::vector<char> stored = shader ? std::vector<char>{} : read_cached_shader(file);
        if (!shader && !stored.empty()) g_device->CreatePixelShader(stored.data(), stored.size(), nullptr, &shader);
        if (shader) return (g_pixel_shaders[key] = shader).Get();
        pending = g_pixel_pending
                      .emplace(key, std::async(std::launch::async, [source = std::move(source), file = std::move(file)] {
                                        return compile_pixel_shader(source, file);
                                    }).share())
                      .first;
    }
    const uint64_t start = ticks();
    const double left = std::max(kCompileWaitMs - g_compile_waited_ms, 0.0);
    const bool ready =
        pending->second.wait_for(std::chrono::duration<double, std::milli>(left)) == std::future_status::ready;
    g_compile_waited_ms += milliseconds(ticks() - start);
    if (!ready) {
        ++g_stats.shader_waits;
        return nullptr;  // the draw is left out this time
    }
    ComPtr<ID3D11PixelShader> shader = pending->second.get();
    g_pixel_pending.erase(pending);
    return (g_pixel_shaders[key] = shader).Get();  // null if it failed to compile: its draws stay out, as before
}

// <GAME>_DUMP_FRAMES=<n>: every n-th presented frame is read back and saved as frame-<count>.bmp next to the log,
// to check the rendering without looking at the screen.
uint32_t g_dump_every = 0;
uint64_t g_presented = 0;

// <GAME>_DUMP_TARGET=<hex address>: with each dumped frame, also the render target at that physical address.
uint32_t g_dump_target = 0;

void dump_frame(ID3D11Texture2D* texture, const char* prefix = "frame") {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &staging); FAILED(hr)) {
        log_resource_failure("a staging copy", hr, desc);
        return;
    }
    g_context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    char name[64];
    std::snprintf(name, sizeof(name), "%s-%05llu.bmp", prefix, static_cast<unsigned long long>(g_presented));
    if (FILE* file = std::fopen(name, "wb")) {
        const uint32_t row = desc.Width * 4;
        BITMAPFILEHEADER header{};
        BITMAPINFOHEADER info{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(header) + sizeof(info);
        header.bfSize = header.bfOffBits + row * desc.Height;
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(desc.Width);
        info.biHeight = -static_cast<LONG>(desc.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        std::fwrite(&header, sizeof(header), 1, file);
        std::fwrite(&info, sizeof(info), 1, file);
        // Float targets (signed blending) as 8-bit BGRA, clamped, alpha kept: a light mask counted in alpha shows.
        const auto half = [](uint16_t h) {
            const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
            const float magnitude = exponent ? std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15)
                                             : std::ldexp(mantissa / 1024.0f, -14);
            return (h & 0x8000) ? -magnitude : magnitude;
        };
        const auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
        std::vector<uint8_t> converted(row);
        std::vector<float> alphas;  // a float target's alpha as it is, signed, in <name>.alpha (float32, rows)
        float alpha_low = 0, alpha_high = 0;
        for (uint32_t y = 0; y < desc.Height; ++y) {
            const uint8_t* line = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
            if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const auto* rgba = reinterpret_cast<const uint16_t*>(line);
                for (uint32_t x = 0; x < desc.Width; ++x) {
                    converted[x * 4 + 0] = byte(half(rgba[x * 4 + 2]));
                    converted[x * 4 + 1] = byte(half(rgba[x * 4 + 1]));
                    converted[x * 4 + 2] = byte(half(rgba[x * 4 + 0]));
                    converted[x * 4 + 3] = byte(half(rgba[x * 4 + 3]));
                    alpha_low = std::min(alpha_low, half(rgba[x * 4 + 3]));
                    alpha_high = std::max(alpha_high, half(rgba[x * 4 + 3]));
                    alphas.push_back(half(rgba[x * 4 + 3]));
                }
                line = converted.data();
            }
            std::fwrite(line, row, 1, file);
        }
        std::fclose(file);
        if (!alphas.empty()) {
            if (FILE* raw = std::fopen((std::string(name) + ".alpha").c_str(), "wb")) {
                std::fwrite(alphas.data(), sizeof(float), alphas.size(), raw);
                std::fclose(raw);
            }
        }
        log_line("renderer: saved %s (format %u, alpha %g to %g)", name, static_cast<unsigned>(desc.Format), alpha_low,
                 alpha_high);
    }
    g_context->Unmap(staging.Get(), 0);
}

// Opens a texture of the game's device (made with SHARED_KEYEDMUTEX) on the present device, with its keyed mutex as
// each device sees it.
bool share_with_present_device(ID3D11Texture2D* texture, ComPtr<ID3D11Texture2D>& opened,
                               ComPtr<IDXGIKeyedMutex>& game_lock, ComPtr<IDXGIKeyedMutex>& present_lock) {
    ComPtr<IDXGIResource> shared;
    HANDLE handle = nullptr;
    return SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&shared))) && SUCCEEDED(shared->GetSharedHandle(&handle)) &&
           SUCCEEDED(g_present_device->OpenSharedResource(handle, IID_PPV_ARGS(&opened))) &&
           SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&game_lock))) && SUCCEEDED(opened.As(&present_lock));
}

// The sharing calls can all succeed while the picture never reaches the other device (DXVK answers S_OK to a lock it
// does not have): the window would stop changing. A colour cleared on the game's device is read back on the present
// device, with the locks taken as a frame takes them.
bool shared_picture_arrives(ID3D11Texture2D* texture, ID3D11Texture2D* opened, IDXGIKeyedMutex* game_lock,
                            IDXGIKeyedMutex* present_lock) {
    constexpr DWORD kWaitMs = 1000;  // never INFINITE here: a lock that is not handed over must not hang the start
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g_device->CreateRenderTargetView(texture, nullptr, &rtv)) || game_lock->AcquireSync(0, kWaitMs) != S_OK) {
        return false;
    }
    const float colour[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    g_context->ClearRenderTargetView(rtv.Get(), colour);
    game_lock->ReleaseSync(0);
    D3D11_TEXTURE2D_DESC desc{};
    opened->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_present_device->CreateTexture2D(&desc, nullptr, &staging)) ||
        present_lock->AcquireSync(0, kWaitMs) != S_OK) {
        return false;
    }
    g_present_context->CopyResource(staging.Get(), opened);
    present_lock->ReleaseSync(0);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_present_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto* pixel = static_cast<const uint8_t*>(mapped.pData);  // blue, green, red: the colour above in bytes
    const bool same = std::abs(pixel[0] - 153) <= 2 && std::abs(pixel[1] - 102) <= 2 && std::abs(pixel[2] - 51) <= 2;
    g_present_context->Unmap(staging.Get(), 0);
    return same;
}

// The swap chain follows the window's client area (it changes with resizing and full screen).
void ensure_swap_chain_size() {
    const ForgetBound forget;
    RECT client{};
    GetClientRect(g_window_handle, &client);
    const auto width = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
    const auto height = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
    if (width == g_swap_width && height == g_swap_height && g_final_rtv) return;
    std::scoped_lock lock(g_swap_mutex, g_present_mutex);
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_final_rtv.Reset();
    g_final_texture.Reset();
    g_final_lock.Reset();
    g_present_source.Reset();
    g_present_lock.Reset();
    g_back_buffer.Reset();
    g_swap_chain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, g_swap_flags);
    g_swap_width = width;
    g_swap_height = height;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&g_back_buffer));
    D3D11_TEXTURE2D_DESC desc{};
    g_back_buffer->GetDesc(&desc);
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = g_present_threaded ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0;
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &g_final_texture); FAILED(hr)) {
        log_resource_failure("the window's picture", hr, desc);
        return;
    }
    g_device->CreateRenderTargetView(g_final_texture.Get(), nullptr, &g_final_rtv);
    if (!g_present_threaded) {
        g_present_source = g_final_texture;
        return;
    }
    if (!share_with_present_device(g_final_texture.Get(), g_present_source, g_final_lock, g_present_lock)) {
        log_line("renderer: the present device cannot open the window's picture");
        g_present_source.Reset();
        g_final_lock.Reset();
        g_present_lock.Reset();
    }
}

// --- modern effects ---
//
// Bloom: the bright parts (a soft threshold) at a quarter and an eighth of the frame's size, each blurred
// (horizontal then vertical Gaussian): a narrow and a wide glow, laid over the picture as light is (screen blend: it
// brightens without going past white). Contrast is an S curve around mid grey that keeps black black and white white
// (a straight line clipped the highlights and crushed the night's dark parts), then saturation, then the corners a
// little darker. All this at the end of the 3D scene, before the HUD is drawn over it (apply_scene_effects), so the
// HUD keeps its own colours; a frame without a 3D scene (films, menus) gets it when it is presented. Sharpening
// (contrast-adaptive, like AMD CAS) is always at present. Settings in <port>.ini; F10 switches the effects off and
// on to compare.

struct PostConstants {
    float source_texel[2];  // 1 / size of the texture read
    float output_texel[2];  // 1 / size of the frame's rectangle in the window
    float sharpen, bloom, bloom_threshold, saturation;
    float contrast, vignette, direction_x, direction_y;  // blur direction (in source texels)
    float grade, unused[3];  // 1: bloom, contrast, saturation and vignette; 0: sharpening only (graded at scene end)
};

ComPtr<ID3D11Buffer> g_post_constants;
ComPtr<ID3D11PixelShader> g_bright_pixel_shader, g_blur_pixel_shader, g_final_pixel_shader;
ComPtr<ID3D11Texture2D> g_bloom_texture[4];  // a quarter of the frame (0, 1) and an eighth (2, 3)
ComPtr<ID3D11RenderTargetView> g_bloom_rtv[4];
ComPtr<ID3D11ShaderResourceView> g_bloom_srv[4];
ComPtr<ID3D11BlendState> g_colour_only_blend;  // writes red, green and blue: the frame's alpha is the HUD's mask
uint32_t g_bloom_width = 0, g_bloom_height = 0;

constexpr char kBrightPixelShader[] = R"(
cbuffer Post : register(b0) { float2 source_texel; float2 output_texel; float sharpen, bloom, threshold, saturation;
                              float contrast, vignette; float2 direction; float grade; float3 unused; };
Texture2D frame : register(t0);
SamplerState filtered : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // A quarter-size texel covers 4x4 frame texels: four bilinear taps average them.
    float3 sum = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float2 offset = float2((i & 1) ? 1 : -1, (i & 2) ? 1 : -1) * source_texel;
        sum += frame.SampleLevel(filtered, uv + offset, 0).rgb;
    }
    const float3 colour = sum / 4;
    // A soft threshold: from (threshold - knee) the bright part grows smoothly instead of starting with a step.
    const float brightness = max(colour.r, max(colour.g, colour.b));
    const float knee = 0.15;
    const float soft = clamp(brightness - threshold + knee, 0, 2 * knee);
    const float bright = max(soft * soft / (4 * knee), brightness - threshold);
    return float4(colour * saturate(bright / max(brightness, 1e-4) / max(1 - threshold, 0.05)), 1);
}
)";

constexpr char kBlurPixelShader[] = R"(
cbuffer Post : register(b0) { float2 source_texel; float2 output_texel; float sharpen, bloom, threshold, saturation;
                              float contrast, vignette; float2 direction; float grade; float3 unused; };
Texture2D source : register(t0);
SamplerState filtered : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // 9-tap Gaussian folded into 5 bilinear taps.
    const float weights[3] = {0.2270270270, 0.3162162162, 0.0702702703};
    const float offsets[3] = {0.0, 1.3846153846, 3.2307692308};
    const float2 step = direction * source_texel;
    float3 sum = source.SampleLevel(filtered, uv, 0).rgb * weights[0];
    [unroll] for (int i = 1; i < 3; ++i) {
        sum += source.SampleLevel(filtered, uv + step * offsets[i], 0).rgb * weights[i];
        sum += source.SampleLevel(filtered, uv - step * offsets[i], 0).rgb * weights[i];
    }
    return float4(sum, 1);
}
)";

constexpr char kFinalPixelShader[] = R"(
cbuffer Post : register(b0) { float2 source_texel; float2 output_texel; float sharpen, bloom, threshold, saturation;
                              float contrast, vignette; float2 direction; float grade; float3 unused; };
Texture2D frame : register(t0);
Texture2D glow : register(t1);
SamplerState filtered : register(s0);
Texture2D wide_glow : register(t2);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = frame.SampleLevel(filtered, uv, 0).rgb;
    if (sharpen > 0) {
        // Contrast-adaptive sharpening on the cross of neighbouring output pixels.
        const float3 n = frame.SampleLevel(filtered, uv + float2(0, -output_texel.y), 0).rgb;
        const float3 s = frame.SampleLevel(filtered, uv + float2(0, output_texel.y), 0).rgb;
        const float3 w = frame.SampleLevel(filtered, uv + float2(-output_texel.x, 0), 0).rgb;
        const float3 e = frame.SampleLevel(filtered, uv + float2(output_texel.x, 0), 0).rgb;
        const float3 low = min(c, min(min(n, s), min(w, e)));
        const float3 high = max(c, max(max(n, s), max(w, e)));
        const float3 amplitude = sqrt(saturate(min(low, 1 - high) / max(high, 1e-4)));
        const float3 peak = -1 / lerp(8.0, 5.0, saturate(sharpen));
        const float3 weight = amplitude * peak;
        c = saturate((c + (n + s + w + e) * weight) / (1 + 4 * weight));
    }
    if (grade > 0) {
        const float3 light = (glow.SampleLevel(filtered, uv, 0).rgb + wide_glow.SampleLevel(filtered, uv, 0).rgb) * 0.5;
        c = 1 - (1 - saturate(c)) * (1 - saturate(light * bloom));  // screen: light added, never past white
        // Contrast as a curve through mid grey with that slope there, flattening towards black and white.
        const float3 up = pow(c, contrast), down = pow(1 - c, contrast);
        c = up / max(up + down, 1e-5);
        const float luma = dot(c, float3(0.299, 0.587, 0.114));
        c = lerp(luma.xxx, c, saturation);
        const float2 centred = uv - 0.5;
        c *= 1 - vignette * smoothstep(0.2, 0.75, dot(centred, centred) * 2);
    }
    return float4(saturate(c), 1);
}
)";

void ensure_bloom_targets(uint32_t width, uint32_t height) {
    const ForgetBound forget;
    width = std::max(2u, width / 4), height = std::max(2u, height / 4);
    if (g_bloom_texture[0] && g_bloom_width == width && g_bloom_height == height) return;
    for (int i = 0; i < 4; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = i < 2 ? width : width / 2;
        desc.Height = i < 2 ? height : height / 2;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        g_bloom_texture[i].Reset();
        g_bloom_rtv[i].Reset();
        g_bloom_srv[i].Reset();
        g_device->CreateTexture2D(&desc, nullptr, &g_bloom_texture[i]);
        g_device->CreateRenderTargetView(g_bloom_texture[i].Get(), nullptr, &g_bloom_rtv[i]);
        g_device->CreateShaderResourceView(g_bloom_texture[i].Get(), nullptr, &g_bloom_srv[i]);
    }
    g_bloom_width = width;
    g_bloom_height = height;
}

// One full-surface pass into `rtv` (viewport `area`) reading `sources`.
// `blend` and `clip` (a scissor inside `area`) are optional.
void post_pass(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& area, ID3D11PixelShader* shader,
               ID3D11ShaderResourceView* const* sources, UINT source_count, const PostConstants& constants,
               ID3D11BlendState* blend = nullptr, const D3D11_RECT* clip = nullptr) {
    const ForgetBound forget;
    g_context->UpdateSubresource(g_post_constants.Get(), 0, nullptr, &constants, 0, 0);
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    g_context->RSSetViewports(1, &area);
    const D3D11_RECT scissor = clip ? *clip
                                    : D3D11_RECT{static_cast<LONG>(area.TopLeftX), static_cast<LONG>(area.TopLeftY),
                                                 static_cast<LONG>(area.TopLeftX + area.Width),
                                                 static_cast<LONG>(area.TopLeftY + area.Height)};
    g_context->RSSetScissorRects(1, &scissor);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_present_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(shader, nullptr, 0);
    ID3D11Buffer* buffer = g_post_constants.Get();
    g_context->PSSetConstantBuffers(0, 1, &buffer);
    g_context->PSSetShaderResources(0, source_count, sources);
    ID3D11SamplerState* sampler = g_present_sampler.Get();
    g_context->PSSetSamplers(0, 1, &sampler);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {};
    g_context->PSSetShaderResources(0, 3, none);
}

// The two glows of `frame` (bloom setting > 0): into g_bloom_srv[0] (a quarter, narrow) and [3] (an eighth, wide).
void make_glow(ID3D11ShaderResourceView* frame, uint32_t frame_width, uint32_t frame_height, PostConstants constants) {
    ensure_bloom_targets(frame_width, frame_height);
    const D3D11_VIEWPORT quarter{0, 0, static_cast<float>(g_bloom_width), static_cast<float>(g_bloom_height), 0, 1};
    const D3D11_VIEWPORT eighth{0, 0, static_cast<float>(g_bloom_width / 2), static_cast<float>(g_bloom_height / 2), 0, 1};
    constants.source_texel[0] = 1.0f / frame_width, constants.source_texel[1] = 1.0f / frame_height;
    post_pass(g_bloom_rtv[0].Get(), quarter, g_bright_pixel_shader.Get(), &frame, 1, constants);
    constants.source_texel[0] = 1.0f / g_bloom_width, constants.source_texel[1] = 1.0f / g_bloom_height;
    ID3D11ShaderResourceView* source = g_bloom_srv[0].Get();
    constants.direction_x = 1, constants.direction_y = 0;
    post_pass(g_bloom_rtv[1].Get(), quarter, g_blur_pixel_shader.Get(), &source, 1, constants);
    source = g_bloom_srv[1].Get();
    constants.direction_x = 0, constants.direction_y = 1;
    post_pass(g_bloom_rtv[0].Get(), quarter, g_blur_pixel_shader.Get(), &source, 1, constants);
    // The wide one: the narrow glow, blurred again at half its size with steps twice as long.
    source = g_bloom_srv[0].Get();
    constants.direction_x = 2, constants.direction_y = 0;
    post_pass(g_bloom_rtv[2].Get(), eighth, g_blur_pixel_shader.Get(), &source, 1, constants);
    constants.source_texel[0] = 2.0f / g_bloom_width, constants.source_texel[1] = 2.0f / g_bloom_height;
    source = g_bloom_srv[2].Get();
    constants.direction_x = 0, constants.direction_y = 2;
    post_pass(g_bloom_rtv[3].Get(), eighth, g_blur_pixel_shader.Get(), &source, 1, constants);
}

PostConstants effect_settings() {
    PostConstants constants{};
    constants.sharpen = setting("sharpen");
    constants.bloom = setting("bloom");
    constants.bloom_threshold = setting("bloom_threshold");
    constants.saturation = setting("saturation");
    constants.contrast = std::max(0.1f, setting("contrast"));
    constants.vignette = setting("vignette");
    constants.grade = 1;
    return constants;
}

ID3D11ShaderResourceView* target_copy(const Target& target);  // below, with the draws

// At the end of a shown surface's 3D scene, under the HUD: bloom, contrast, saturation and vignette, within the
// scene's rectangle (a split-screen view gets them once, when its own scene ends). Alpha is left as it is.
void apply_scene_effects(Target& target, const SceneState& scene, uint32_t address) {
    const ForgetBound forget;
    if (!g_effects_enabled || !g_final_pixel_shader || !g_colour_only_blend) return;
    D3D11_TEXTURE2D_DESC desc{};
    target.texture->GetDesc(&desc);
    ID3D11ShaderResourceView* copy = target_copy(target);
    if (!copy) return;
    PostConstants constants = effect_settings();
    constants.sharpen = 0;  // at present, over the HUD too
    if (constants.bloom > 0) make_glow(copy, desc.Width, desc.Height, constants);
    ID3D11ShaderResourceView* sources[3] = {copy, constants.bloom > 0 ? g_bloom_srv[0].Get() : nullptr,
                                            constants.bloom > 0 ? g_bloom_srv[3].Get() : nullptr};
    const D3D11_VIEWPORT whole{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    D3D11_RECT clip{scaled(scene.x0), scaled(scene.y0), scaled(scene.x1), scaled(scene.y1)};
    if (clip.right <= clip.left || clip.bottom <= clip.top) {
        clip = D3D11_RECT{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    }
    constants.output_texel[0] = 1.0f / desc.Width, constants.output_texel[1] = 1.0f / desc.Height;
    post_pass(target.rtv.Get(), whole, g_final_pixel_shader.Get(), sources, 3, constants, g_colour_only_blend.Get(), &clip);
    g_graded_surfaces.insert(address);
}

// --- DLSS, an experiment (<GAME>_DLSS=dlaa|quality|balanced|performance) ---
//
// The finished frame (internal resolution, HUD included) upscaled to the window's picture by NVIDIA DLSS in the 64-bit
// helper (upscale_bridge.h, tools/upscale64 next to the game's exe, or <GAME>_UPSCALE64), instead of the bilinear
// stretch. For measuring and trying it: no jitter and no motion vectors yet (flat depth, still vectors), the HUD goes
// through DLSS too. The plan's way (the scene alone, jittered, with vectors) is in sick-xport
// docs/UPSCALING-DLSS-FSR.md. A frame behind: each present sends its frame and shows the one the helper made from the
// frame before (copied out of the shared texture), so neither waits for the other.
struct DlssPresent {
    UpscaleBridge bridge;
    uint32_t input_width = 0, input_height = 0, output_width = 0, output_height = 0;
    ComPtr<ID3D11Texture2D> shown;  // the helper's last output, ours to draw
    ComPtr<ID3D11ShaderResourceView> shown_view;
    ComPtr<ID3D11RenderTargetView> depth, motion;
    bool failed = false, fresh = false, pending = false, has_shown = false;
    double waited_ms = 0;  // for the helper's output and for the inputs back, since the last log line
    uint32_t frames = 0;
};
DlssPresent g_dlss;

// Experimental: only with upscale64.exe beside the game's exe (not in the release packages yet), so a setting without
// it changes nothing.
bool dlss_helper_present() {
    static const bool present = [] {
        std::error_code ignored;
        return std::filesystem::exists(program_directory() / L"upscale64.exe", ignored);
    }();
    return present;
}

// The settings upscaler (2: DLSS) and upscaler_quality (0 DLAA, 1 Quality, 2 Balanced, 3 Performance), read at start;
// <GAME>_DLSS=<mode> for tests (it keeps render_scale as it is).
const char* dlss_mode() {
    static const char* mode = []() -> const char* {
        static char asked[32] = {};
        if (GetEnvironmentVariableA(XPORT_ENV("DLSS"), asked, sizeof(asked)) > 0) return asked[0] && asked[0] != '0' ? asked : nullptr;
        if (setting_at_start("upscaler") != 2 || !dlss_helper_present()) return nullptr;
        static const char* const kModes[] = {"dlaa", "quality", "balanced", "performance"};
        return kModes[std::clamp(static_cast<int>(setting_at_start("upscaler_quality")), 0, 3)];
    }();
    return mode;
}

// The screen's lines over the drawn ones for the setting's profile (DLSS's own: Quality 1.5, Balanced 1.72,
// Performance 2), or 0 when the setting asks for no upscaling.
float upscaler_ratio() {
    if (setting_at_start("upscaler") != 2 || !dlss_helper_present()) return 0;
    static const float kRatios[] = {1.0f, 1.5f, 1.72f, 2.0f};
    return kRatios[std::clamp(static_cast<int>(setting_at_start("upscaler_quality")), 0, 3)];
}

// `frame` sent to DLSS for output_width x output_height; the view of the picture it made from the frame before, or
// null (the first frame; no DLSS asked, or it cannot run: said once in the log).
ID3D11ShaderResourceView* dlss_upscale(ID3D11ShaderResourceView* frame, uint32_t input_width, uint32_t input_height,
                                       uint32_t output_width, uint32_t output_height) {
    if (!dlss_mode() || g_dlss.failed || !frame || !input_width || !output_width) return nullptr;
    ComPtr<ID3D11Resource> source;
    frame->GetResource(&source);
    ComPtr<ID3D11Texture2D> source_texture;
    D3D11_TEXTURE2D_DESC source_desc{};
    if (FAILED(source.As(&source_texture))) return nullptr;
    source_texture->GetDesc(&source_desc);
    if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        log_line("renderer: DLSS: the frame is not BGRA8 (format %u), not upscaled", static_cast<unsigned>(source_desc.Format));
        g_dlss.failed = true;
        return nullptr;
    }
    if (!g_dlss.shown || g_dlss.input_width != input_width || g_dlss.input_height != input_height ||
        g_dlss.output_width != output_width || g_dlss.output_height != output_height) {
        g_dlss.bridge.stop();
        g_dlss.shown.Reset(), g_dlss.shown_view.Reset(), g_dlss.depth.Reset(), g_dlss.motion.Reset();
        g_dlss.pending = g_dlss.has_shown = false;
        wchar_t path[MAX_PATH] = {};
        const std::wstring helper = GetEnvironmentVariableW(XPORT_ENV_W("UPSCALE64"), path, MAX_PATH)
                                        ? std::wstring(path)
                                        : (program_directory() / L"upscale64.exe").wstring();
        g_dlss.bridge.set_log([](const char* line) { log_line("%s", line); });
        const uint64_t started = ticks();
        if (!g_dlss.bridge.start(g_device.Get(), helper, input_width, input_height, output_width, output_height, dlss_mode())) {
            log_line("renderer: DLSS did not start (%s): the bilinear stretch stays", g_dlss.bridge.error().c_str());
            g_dlss.failed = true;
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC shown_desc{};
        g_dlss.bridge.output()->GetDesc(&shown_desc);
        shown_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        shown_desc.MiscFlags = 0;
        g_device->CreateTexture2D(&shown_desc, nullptr, &g_dlss.shown);
        if (g_dlss.shown) g_device->CreateShaderResourceView(g_dlss.shown.Get(), nullptr, &g_dlss.shown_view);
        g_device->CreateRenderTargetView(g_dlss.bridge.depth(), nullptr, &g_dlss.depth);
        g_device->CreateRenderTargetView(g_dlss.bridge.motion(), nullptr, &g_dlss.motion);
        g_dlss.input_width = input_width, g_dlss.input_height = input_height;
        g_dlss.output_width = output_width, g_dlss.output_height = output_height;
        g_dlss.fresh = true;
        log_line("renderer: DLSS %s, %ux%u to %ux%u (helper started in %.0f ms)", dlss_mode(), input_width, input_height,
                 output_width, output_height, milliseconds(ticks() - started));
    }
    const uint64_t before = ticks();
    // The picture the helper made from the last frame: ours to show from now on.
    if (g_dlss.pending) {
        if (!g_dlss.bridge.receive(500)) {
            log_line("renderer: DLSS: no output from the helper: the bilinear stretch stays");
            g_dlss.failed = true;
            return nullptr;
        }
        g_context->CopyResource(g_dlss.shown.Get(), g_dlss.bridge.output());
        g_dlss.bridge.release_output();
        g_dlss.pending = false;
        g_dlss.has_shown = true;
    }
    // This frame to the helper (the inputs came back with that output).
    if (!g_dlss.bridge.acquire_input(200)) {
        log_line("renderer: DLSS: the helper did not give the input back: the bilinear stretch stays");
        g_dlss.failed = true;
        return nullptr;
    }
    g_dlss.waited_ms += milliseconds(ticks() - before);
    const D3D11_BOX box{0, 0, 0, input_width, input_height, 1};
    g_context->CopySubresourceRegion(g_dlss.bridge.colour(), 0, 0, 0, 0, source_texture.Get(), 0, &box);
    if (g_dlss.fresh) {  // flat depth and no motion, once: they stay
        const float flat[4] = {0.5f, 0, 0, 0}, still[4] = {0, 0, 0, 0};
        g_context->ClearRenderTargetView(g_dlss.depth.Get(), flat);
        g_context->ClearRenderTargetView(g_dlss.motion.Get(), still);
    }
    UpscaleFrame frame_data;
    frame_data.reset = g_dlss.fresh;
    g_dlss.fresh = false;
    if (!g_dlss.bridge.send(g_context.Get(), frame_data)) {
        log_line("renderer: DLSS: the helper could not take the frame: the bilinear stretch stays");
        g_dlss.failed = true;
        return nullptr;
    }
    g_dlss.pending = true;
    ++g_dlss.frames;
    return g_dlss.has_shown ? g_dlss.shown_view.Get() : nullptr;
}

// Draws the frame into the window: the largest rectangle of the picture's shape (renderer_aspect) that fits,
// centred, filtered, with the effects.
void present_frame(ID3D11ShaderResourceView* frame, uint32_t frame_width, uint32_t frame_height, bool graded) {
    const ForgetBound forget;
    if (!g_final_rtv || !g_present_pixel_shader) return;
    if (dlss_mode()) {
        const float aspect = g_shown_aspect > 0 ? g_shown_aspect : renderer_aspect();
        float width = static_cast<float>(g_swap_width), height = static_cast<float>(g_swap_height);
        if (width > height * aspect) width = height * aspect;
        else height = width / aspect;
        if (ID3D11ShaderResourceView* upscaled = dlss_upscale(frame, frame_width, frame_height, static_cast<uint32_t>(width),
                                                              static_cast<uint32_t>(height))) {
            frame = upscaled, frame_width = g_dlss.output_width, frame_height = g_dlss.output_height;
        }
    }
    if (g_effects_enabled && g_final_pixel_shader && frame_width && frame_height) {
        const float aspect = g_shown_aspect > 0 ? g_shown_aspect : renderer_aspect();
        float width = static_cast<float>(g_swap_width), height = static_cast<float>(g_swap_height);
        if (width > height * aspect) {
            width = height * aspect;
        } else {
            height = width / aspect;
        }
        const D3D11_VIEWPORT area{std::floor((g_swap_width - width) / 2), std::floor((g_swap_height - height) / 2),
                                  width, height, 0, 1};
        PostConstants constants = effect_settings();
        if (graded) constants.grade = 0;  // the scene has them already, under the HUD
        const bool glow = constants.grade > 0 && constants.bloom > 0;
        if (glow) make_glow(frame, frame_width, frame_height, constants);
        const float black[4] = {0, 0, 0, 1};
        g_context->ClearRenderTargetView(g_final_rtv.Get(), black);
        constants.output_texel[0] = 1.0f / width, constants.output_texel[1] = 1.0f / height;
        ID3D11ShaderResourceView* sources[3] = {frame, glow ? g_bloom_srv[0].Get() : nullptr,
                                                glow ? g_bloom_srv[3].Get() : nullptr};
        post_pass(g_final_rtv.Get(), area, g_final_pixel_shader.Get(), sources, 3, constants);
        return;
    }
    const float aspect = g_shown_aspect > 0 ? g_shown_aspect : renderer_aspect();
    float width = static_cast<float>(g_swap_width), height = static_cast<float>(g_swap_height);
    if (width > height * aspect) {
        width = height * aspect;
    } else {
        height = width / aspect;
    }
    const float left = std::floor((g_swap_width - width) / 2), top = std::floor((g_swap_height - height) / 2);
    const float black[4] = {0, 0, 0, 1};
    g_context->ClearRenderTargetView(g_final_rtv.Get(), black);
    ID3D11RenderTargetView* rtv = g_final_rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{left, top, width, height, 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(g_swap_width), static_cast<LONG>(g_swap_height)};
    g_context->RSSetScissorRects(1, &scissor);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_present_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_present_pixel_shader.Get(), nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &frame);
    ID3D11SamplerState* sampler = g_present_sampler.Get();
    g_context->PSSetSamplers(0, 1, &sampler);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
}

// Guest pixels (32-bit) into a region of an enlarged render target: through a small texture drawn g_scale times
// larger, since a copy cannot stretch.
ComPtr<ID3D11PixelShader> g_stretch_pixel_shader;
ComPtr<ID3D11SamplerState> g_point_sampler;
ComPtr<ID3D11Texture2D> g_stretch_source;
ComPtr<ID3D11ShaderResourceView> g_stretch_source_srv;
uint32_t g_stretch_width = 0, g_stretch_height = 0;

void upload_scaled(Target& to, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* pixels,
                   uint32_t pitch) {
    if (!pixels || !g_stretch_pixel_shader) return;
    const ForgetBound forget;  // binds its own target, shaders, texture and sampler
    if (!g_stretch_source || g_stretch_width != width || g_stretch_height != height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_stretch_source.Reset();
        g_stretch_source_srv.Reset();
        if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &g_stretch_source); FAILED(hr)) {
            log_resource_failure("the stretch source", hr, desc);
            return;
        }
        g_device->CreateShaderResourceView(g_stretch_source.Get(), nullptr, &g_stretch_source_srv);
        g_stretch_width = width;
        g_stretch_height = height;
    }
    g_context->UpdateSubresource(g_stretch_source.Get(), 0, nullptr, pixels, pitch, 0);
    ID3D11RenderTargetView* rtv = to.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{scaled_float(x), scaled_float(y), scaled_float(width), scaled_float(height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{scaled(x), scaled(y), scaled(x + width), scaled(y + height)};
    g_context->RSSetScissorRects(1, &scissor);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_present_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_stretch_pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* source = g_stretch_source_srv.Get();
    g_context->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState* sampler = g_point_sampler.Get();
    g_context->PSSetSamplers(0, 1, &sampler);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
}

// Copies the finished picture into the swap chain and presents it: on the present thread with the swap chain's own
// device, or on the GPU thread when there is no second device.
void present_now() {
    std::lock_guard<std::mutex> swap(g_swap_mutex);
    {
        std::lock_guard<std::mutex> drawing(g_present_mutex);
        if (!g_back_buffer || !g_present_source) return;
        if (g_present_lock) g_present_lock->AcquireSync(0, INFINITE);
        g_present_context->CopyResource(g_back_buffer.Get(), g_present_source.Get());
        if (g_present_lock) g_present_lock->ReleaseSync(0);
    }
    // <GAME>_TEST_PRESENT_HOOK=1: acts as an overlay tool hooked on Present does (RTSS, Steam, Discord): it takes the
    // swap chain's device, saves the target, blend and pixel shader textures, binds its own and puts the saved ones
    // back. On the game's device that broke the city's textures (issue #6); on the present device it shows nothing.
    static const bool test_hook = GetEnvironmentVariableW(XPORT_ENV_W("TEST_PRESENT_HOOK"), nullptr, 0) != 0;
    if (test_hook) {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        g_swap_chain->GetDevice(IID_PPV_ARGS(&device));
        device->GetImmediateContext(&context);
        ID3D11ShaderResourceView* saved[8] = {};
        ID3D11ShaderResourceView* none[8] = {};
        ComPtr<ID3D11BlendState> blend;
        float factor[4];
        UINT mask = 0;
        context->PSGetShaderResources(0, 8, saved);
        context->OMGetBlendState(&blend, factor, &mask);
        ID3D11RenderTargetView* target = nullptr;
        ID3D11DepthStencilView* depth = nullptr;
        context->OMGetRenderTargets(1, &target, &depth);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->PSSetShaderResources(0, 8, none);
        context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        Sleep(3);
        context->PSSetShaderResources(0, 8, saved);
        context->OMSetBlendState(blend.Get(), factor, mask);
        context->OMSetRenderTargets(1, &target, depth);
        if (target) target->Release();
        if (depth) depth->Release();
        for (ID3D11ShaderResourceView* view : saved) {
            if (view) view->Release();
        }
    }
    const uint64_t start = ticks();
    const HRESULT presented = g_swap_chain->Present(0, g_swap_flags ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) device_lost(presented);
    const uint64_t taken = ticks() - start;
    {
        std::lock_guard<std::mutex> drawing(g_present_mutex);  // read by log_statistics on the GPU thread
        g_stats.swap_ticks += taken;
    }
    static int slow_presents = 0;
    if (milliseconds(taken) > 250 && slow_presents < 20) {
        ++slow_presents;
        log_line("renderer: a Present took %.0f ms", milliseconds(taken));
    }
}

void present_thread() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_present_mutex);
            g_present_wake.wait(lock, [] { return g_present_pending; });
            g_present_pending = false;
        }
        present_now();
    }
}

}  // namespace

bool renderer_start(HWND window) {
    // render_scale 0 (Automatic): as many lines as the window's monitor has, for a picture that is not resampled
    // up or down in height. The game draws 480; 2160 (4K) at most.
    const float requested = setting_at_start("render_scale");
    float scale = requested;
    const float ratio = upscaler_ratio();
    if (requested <= 0 || ratio > 0) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
        scale = static_cast<float>(monitor.rcMonitor.bottom - monitor.rcMonitor.top) / 480.0f;
        // Upscaling: the profile's share of the screen's lines, which DLSS brings back to the window.
        if (ratio > 0) scale /= ratio;
    }
    // On laptops with two GPUs Windows' default adapter is the integrated one: ask DXGI 1.6 for the fast one instead
    // (the player's choice for the game's exe in Windows' graphics settings still wins). <GAME>_GPU_PREFERENCE=0: Windows' default.
    ComPtr<IDXGIAdapter> preferred;
    ComPtr<IDXGIFactory6> factory6;
    wchar_t preference[8] = {};
    GetEnvironmentVariableW(XPORT_ENV_W("GPU_PREFERENCE"), preference, 8);
    if (preference[0] != L'0' && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory6)))) {
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&preferred));
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(preferred.Get(), preferred ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &g_device,
                                 nullptr, &g_context))) {
        log_line("renderer: no Direct3D 11 device");
        return false;
    }
    if (preferred) {
        DXGI_ADAPTER_DESC desc{};
        preferred->GetDesc(&desc);
        log_line("renderer: on %ls", desc.Description);
    }
    // Automatic also means within the GPU's means: integrated graphics (under 1 GB of their own memory) get 1080
    // lines at most. The description's size is 32-bit here and large cards wrap, so the 64-bit budget decides for them.
    if (requested <= 0) {
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIAdapter3> adapter3;
        DXGI_ADAPTER_DESC desc{};
        DXGI_QUERY_VIDEO_MEMORY_INFO budget{};
        constexpr uint64_t kGigabyte = 1ull << 30;
        if (SUCCEEDED(g_device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&desc)) && desc.DedicatedVideoMemory < kGigabyte &&
            (FAILED(adapter.As(&adapter3)) ||
             FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &budget)) ||
             budget.Budget < kGigabyte)) {
            scale = std::min(scale, 1080.0f / 480);
        }
    }
    g_scale = std::clamp(scale, 1.0f, 6.0f);
    log_line("renderer: internal resolution %.0f lines (%.3gx)%s", 480.0f * g_scale, g_scale,
             ratio > 0 ? ", the upscaling profile's" : requested <= 0 ? ", automatic" : "");
    g_context.As(&g_context1);

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    g_device.As(&dxgi_device);
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    // The swap chain's own device, for the present thread (g_present_device), if the window's picture can be shared
    // with it: tried here on a small texture, since a failure later would leave the window black for good.
    if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    levels, 1, D3D11_SDK_VERSION, &g_present_device, nullptr, &g_present_context))) {
        D3D11_TEXTURE2D_DESC probe_desc{};
        probe_desc.Width = probe_desc.Height = 16;
        probe_desc.MipLevels = probe_desc.ArraySize = 1;
        probe_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        probe_desc.SampleDesc.Count = 1;
        probe_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        probe_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> probe, opened;
        ComPtr<IDXGIKeyedMutex> game_lock, present_lock;
        g_present_threaded = SUCCEEDED(g_device->CreateTexture2D(&probe_desc, nullptr, &probe)) &&
                             share_with_present_device(probe.Get(), opened, game_lock, present_lock);
        if (g_present_threaded && !shared_picture_arrives(probe.Get(), opened.Get(), game_lock.Get(), present_lock.Get())) {
            log_line("renderer: a picture shared with the second device does not arrive there");
            g_present_threaded = false;
        }
    }
    if (!g_present_threaded) {
        log_line("renderer: no second device for presenting, presenting on the GPU thread");
        g_present_device = g_device;
        g_present_context = g_context;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = g_swap_width = 640;
    desc.Height = g_swap_height = 480;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Without it a flip-model Present(0) in a window still waits for the display's next refresh when the frame took
    // longer than one: a frame of 17 ms became 33 ms and the game sat at 30 fps with every thread idle (seen on a
    // laptop whose dedicated GPU draws into a display wired to the integrated one). The port paces frames itself.
    // Setting vsync = 1 gives that up for a picture that never tears (a game without the setting reads 0).
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (setting("vsync") == 0 && SUCCEEDED(factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) &&
        tearing) {
        g_swap_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    desc.Flags = g_swap_flags;
    log_line("renderer: present %s", g_swap_flags ? "without waiting for the display" : "in step with the display");
    if (FAILED(factory->CreateSwapChainForHwnd(g_present_device.Get(), window, &desc, nullptr, nullptr, &g_swap_chain))) {
        log_line("renderer: cannot create the swap chain");
        return false;
    }
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);  // full screen is the window's own (nv2a.cpp)
    g_window_handle = window;
    if (g_present_threaded) std::thread(present_thread).detach();
    start_render_thread();
    preload_shaders();

    ComPtr<ID3DBlob> vs = compile(kVertexShader, sizeof(kVertexShader) - 1, "vs_5_0", "vertex");
    if (!vs) return false;
    g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vertex_shader);
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG", 0, DXGI_FORMAT_R32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    static_assert(sizeof(RenderVertex) == 116);
    g_device->CreateInputLayout(layout, 8, vs->GetBufferPointer(), vs->GetBufferSize(), &g_input_layout);

    const std::string fixed_function = std::string(kVertexDataShader) + kFixedFunctionVertexShader;
    if (ComPtr<ID3DBlob> ff = compile(fixed_function.data(), fixed_function.size(), "vs_5_0", "fixed-function vertex")) {
        g_device->CreateVertexShader(ff->GetBufferPointer(), ff->GetBufferSize(), nullptr, &g_fixed_function_vertex_shader);
    }
    D3D11_BUFFER_DESC transform_constants{};
    transform_constants.ByteWidth = sizeof(FixedFunctionConstants);
    transform_constants.Usage = D3D11_USAGE_DEFAULT;
    transform_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&transform_constants, nullptr, &g_fixed_function_constants);
    transform_constants.ByteWidth = 16;
    g_device->CreateBuffer(&transform_constants, nullptr, &g_fixed_function_draw_constants);
    transform_constants.ByteWidth = sizeof(ProgramConstants);
    g_device->CreateBuffer(&transform_constants, nullptr, &g_program_constants);
    // Writing to a buffer the vertex shader reads, without waiting (NO_OVERWRITE), needs this feature.
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    g_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
    if (!options.MapNoOverwriteOnDynamicBufferSRV) {
        log_line("renderer: no NO_OVERWRITE on shader-read buffers: fixed-function transform stays on the CPU");
        g_fixed_function_vertex_shader.Reset();
    }

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(CombinerConstants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&constants, nullptr, &g_constants);

    // Clears of only some colour channels: a full-surface triangle with the clear colour, written through a mask.
    constexpr char kClearVertexShader[] =
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 p = float2((id << 1) & 2, id & 2);\n"
        "    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";
    constexpr char kClearPixelShader[] =
        "cbuffer Clear : register(b1) { float4 colour; };\n"
        "float4 main() : SV_Target { return colour; }\n";
    ComPtr<ID3DBlob> clear_vs = compile(kClearVertexShader, sizeof(kClearVertexShader) - 1, "vs_5_0", "clear vertex");
    ComPtr<ID3DBlob> clear_ps = compile(kClearPixelShader, sizeof(kClearPixelShader) - 1, "ps_5_0", "clear pixel");
    if (clear_vs && clear_ps) {
        g_device->CreateVertexShader(clear_vs->GetBufferPointer(), clear_vs->GetBufferSize(), nullptr, &g_clear_vertex_shader);
        g_device->CreatePixelShader(clear_ps->GetBufferPointer(), clear_ps->GetBufferSize(), nullptr, &g_clear_pixel_shader);
    }
    constexpr char kCopyPixelShader[] =
        "Texture2D source : register(t0);\n"
        "float4 main(float4 position : SV_Position) : SV_Target { return source.Load(int3(position.xy, 0)); }\n";
    ComPtr<ID3DBlob> copy_ps = compile(kCopyPixelShader, sizeof(kCopyPixelShader) - 1, "ps_5_0", "copy pixel");
    if (copy_ps && g_clear_vertex_shader) {
        g_copy_vertex_shader = g_clear_vertex_shader;
        g_device->CreatePixelShader(copy_ps->GetBufferPointer(), copy_ps->GetBufferSize(), nullptr, &g_copy_pixel_shader);
    }
    constexpr char kPresentVertexShader[] =
        "struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
        "Output main(uint id : SV_VertexID) {\n"
        "    Output o;\n"
        "    const float2 p = float2((id << 1) & 2, id & 2);\n"
        "    o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "    o.uv = p;\n"
        "    return o;\n"
        "}\n";
    constexpr char kPresentPixelShader[] =
        "Texture2D frame : register(t0);\n"
        "SamplerState filtered : register(s0);\n"
        "float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "    return float4(frame.Sample(filtered, uv).rgb, 1);\n"
        "}\n";
    ComPtr<ID3DBlob> present_vs = compile(kPresentVertexShader, sizeof(kPresentVertexShader) - 1, "vs_5_0", "present vertex");
    ComPtr<ID3DBlob> present_ps = compile(kPresentPixelShader, sizeof(kPresentPixelShader) - 1, "ps_5_0", "present pixel");
    if (present_vs && present_ps) {
        g_device->CreateVertexShader(present_vs->GetBufferPointer(), present_vs->GetBufferSize(), nullptr,
                                     &g_present_vertex_shader);
        g_device->CreatePixelShader(present_ps->GetBufferPointer(), present_ps->GetBufferSize(), nullptr,
                                    &g_present_pixel_shader);
    }
    D3D11_SAMPLER_DESC present_sampler{};
    present_sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    present_sampler.AddressU = present_sampler.AddressV = present_sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    present_sampler.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&present_sampler, &g_present_sampler);
    present_sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_device->CreateSamplerState(&present_sampler, &g_point_sampler);
    constexpr char kStretchPixelShader[] =
        "Texture2D source : register(t0);\n"
        "SamplerState nearest : register(s0);\n"
        "float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "    return source.Sample(nearest, uv);\n"
        "}\n";
    const auto pixel = [](const char* source, size_t size, const char* name, ComPtr<ID3D11PixelShader>& shader) {
        if (ComPtr<ID3DBlob> code = compile(source, size, "ps_5_0", name)) {
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
        }
    };
    pixel(kBrightPixelShader, sizeof(kBrightPixelShader) - 1, "bloom bright pixel", g_bright_pixel_shader);
    pixel(kBlurPixelShader, sizeof(kBlurPixelShader) - 1, "bloom blur pixel", g_blur_pixel_shader);
    pixel(kFinalPixelShader, sizeof(kFinalPixelShader) - 1, "final pixel", g_final_pixel_shader);
    pixel(kOcclusionPixelShader, sizeof(kOcclusionPixelShader) - 1, "occlusion pixel", g_occlusion_pixel_shader);
    pixel(kDepthColourPixelShader, sizeof(kDepthColourPixelShader) - 1, "depth as colour pixel",
          g_depth_colour_pixel_shader);
    pixel(kColourToDepthPixelShader, sizeof(kColourToDepthPixelShader) - 1, "colour to depth pixel",
          g_colour_to_depth_pixel_shader);
    D3D11_BUFFER_DESC occlusion_constants{};
    occlusion_constants.ByteWidth = sizeof(OcclusionConstants);
    occlusion_constants.Usage = D3D11_USAGE_DEFAULT;
    occlusion_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&occlusion_constants, nullptr, &g_occlusion_constants);
    D3D11_BLEND_DESC multiply{};
    multiply.RenderTarget[0].BlendEnable = TRUE;
    multiply.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    multiply.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    multiply.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    multiply.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    multiply.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    multiply.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    multiply.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    g_device->CreateBlendState(&multiply, &g_multiply_blend);
    D3D11_BLEND_DESC colour_only{};
    colour_only.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    g_device->CreateBlendState(&colour_only, &g_colour_only_blend);
    D3D11_BUFFER_DESC post_constants{};
    post_constants.ByteWidth = sizeof(PostConstants);
    post_constants.Usage = D3D11_USAGE_DEFAULT;
    post_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&post_constants, nullptr, &g_post_constants);
    g_effects_enabled = setting("effects") != 0;
    ComPtr<ID3DBlob> stretch_ps = compile(kStretchPixelShader, sizeof(kStretchPixelShader) - 1, "ps_5_0", "stretch pixel");
    if (stretch_ps) {
        g_device->CreatePixelShader(stretch_ps->GetBufferPointer(), stretch_ps->GetBufferSize(), nullptr,
                                    &g_stretch_pixel_shader);
    }

    D3D11_BUFFER_DESC surface_constants{};
    surface_constants.ByteWidth = 32;
    surface_constants.Usage = D3D11_USAGE_DEFAULT;
    surface_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&surface_constants, nullptr, &g_surface_constants);
    D3D11_BUFFER_DESC clear_constants{};
    clear_constants.ByteWidth = 16;
    clear_constants.Usage = D3D11_USAGE_DEFAULT;
    clear_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&clear_constants, nullptr, &g_clear_constants);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;  // winding is not translated yet: draw both faces
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = TRUE;  // the surface clip
    g_device->CreateRasterizerState(&raster, &g_rasterizer);
    char dump[16] = {};
    if (GetEnvironmentVariableA(XPORT_ENV("DUMP_FRAMES"), dump, sizeof(dump))) g_dump_every = std::strtoul(dump, nullptr, 10);
    if (GetEnvironmentVariableA(XPORT_ENV("DUMP_TARGET"), dump, sizeof(dump))) g_dump_target = std::strtoul(dump, nullptr, 16);
    log_line("renderer: Direct3D 11 ready");
    return true;
}

bool renderer_over_shown_scene(const SurfaceState& surface) {
    if (g_render_thread_id && GetCurrentThreadId() != g_render_thread_id) {
        const auto queued = g_queued_scenes.find(surface.zeta_address);
        return queued != g_queued_scenes.end() && queued->second && g_shown_surfaces.count(surface.color_address) != 0;
    }
    const auto scene = g_scenes.find(surface.zeta_address);
    return scene != g_scenes.end() && scene->second.drawn && g_shown_surfaces.count(surface.color_address) != 0;
}

namespace {
void clear_now(const SurfaceState& surface, uint32_t flags, uint32_t color, uint32_t depth_stencil, int x0, int y0, int x1,
               int y1) {
    if (!g_device || !surface.width || !surface.height) return;
    Target* target = get_target(surface.color_address, surface.width, surface.height);
    if (!target) return;
    // One more column and row than the rectangle's inclusive maximum: the game's rectangles end a pixel short of the
    // quads it draws over them, and an uncleared edge shows as a dark line beside a mask drawn with destination alpha
    // (MC3's radar clears alpha on 38..165 x 323..450 and draws its map, masked by it, on 39..166 x 324..451).
    x1 += 1, y1 += 1;
    // NV097_CLEAR_SURFACE: R 0x10, G 0x20, B 0x40, A 0x80 map onto D3D11's write-mask bits.
    const uint32_t channels = (flags >> 4) & 0xF;
    renderer_game_clear(surface, channels, x0, y0, x1, y1);
    if (channels == 0xF) {
        float rgba[4];
        unpack_color(color, rgba);
        const D3D11_RECT rect{scaled(x0), scaled(y0), scaled(x1 + 1), scaled(y1 + 1)};
        g_context1->ClearView(target->rtv.Get(), rgba, &rect, 1);
    } else if (channels && g_clear_pixel_shader) {
        // Some channels only (the HUD clears alpha alone to reuse it as a mask).
        const ForgetBound forget;  // binds its own target, blend, scissor and shaders
        float rgba[4];
        unpack_color(color, rgba);
        g_context->UpdateSubresource(g_clear_constants.Get(), 0, nullptr, rgba, 0, 0);
        ComPtr<ID3D11BlendState>& blend = g_clear_blend_states[channels];
        if (!blend) {
            D3D11_BLEND_DESC desc{};
            desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            desc.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(channels);
            g_device->CreateBlendState(&desc, &blend);
        }
        ID3D11RenderTargetView* rtv = target->rtv.Get();
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        g_context->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFF);
        g_context->OMSetDepthStencilState(nullptr, 0);
        const D3D11_VIEWPORT viewport{0, 0, scaled_float(target->width), scaled_float(target->height), 0, 1};
        g_context->RSSetViewports(1, &viewport);
        const D3D11_RECT rect{scaled(x0), scaled(y0), scaled(x1 + 1), scaled(y1 + 1)};
        g_context->RSSetScissorRects(1, &rect);
        g_context->RSSetState(g_rasterizer.Get());
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_clear_vertex_shader.Get(), nullptr, 0);
        g_context->PSSetShader(g_clear_pixel_shader.Get(), nullptr, 0);
        ID3D11Buffer* constants = g_clear_constants.Get();
        g_context->PSSetConstantBuffers(1, 1, &constants);
        g_context->Draw(3, 0);
    }
    // A scene (or a split-screen view) starts. The game also clears the same depth buffer for small targets
    // (256x256, 512x512 reflections) in the middle of a frame; those must not restart the scene, or the occlusion
    // was skipped in some frames and flickered.
    if ((flags & 0x1) && surface.zeta_address && surface.width >= 640) {
        SceneState scene{};
        scene.x0 = x0, scene.y0 = y0, scene.x1 = x1 + 1, scene.y1 = y1 + 1;
        g_scenes[surface.zeta_address] = scene;
        ++g_full_depth_clears;
    }
    if (channels == 0xF || (flags & 0x1)) g_graded_surfaces.erase(surface.color_address);  // a new picture starts
    if ((flags & 0x3) && surface.zeta_address) {
        // The depth buffer has its colour target's size (D3D11 requires it when both are bound).
        if (DepthTarget* depth = get_depth_target(surface.zeta_address, target->width, target->height)) {
            const float z = static_cast<float>(depth_stencil >> 8) / 16777215.0f;
            g_context->ClearDepthStencilView(depth->dsv.Get(),
                                             ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0),
                                             z, static_cast<UINT8>(depth_stencil));
        }
    }
}
}  // namespace

namespace {

// What every draw needs before its vertices: the target (grown or switched to float as needed), the depth buffer,
// the pixel shader, and the ambient occlusion pass when the 3D scene ends.
struct PreparedDraw {
    Target* target = nullptr;
    DepthTarget* depth = nullptr;
    ID3D11PixelShader* shader = nullptr;
};

bool prepare_draw(const SurfaceState& surface, const DrawState& state, PreparedDraw& draw) {
    if (!g_device || !surface.width || !surface.height) return false;
    // Culling both faces draws nothing.
    if (state.cull_enable && state.cull_face == 0x408) return false;
    ++g_stats.draws;
    ++g_total_draws;
    Target* target = get_target(surface.color_address, surface.width, surface.height);
    if (!target) return false;
    if (state.blend_enable && (state.blend_equation == 0xF005 || state.blend_equation == 0xF006)) {
        make_float_target(*target, surface.color_address);
    }
    ID3D11PixelShader* shader = pixel_shader(state.program);
    if (!shader) return false;
    if (g_zeta_addresses.count(surface.color_address)) g_colour_over_zeta.insert(surface.color_address);
    DepthTarget* depth = (surface.zeta_address && (state.depth_test || state.depth_write || state.stencil_test))
                             ? get_depth_target(surface.zeta_address, target->width, target->height)
                             : nullptr;
    if (depth && g_colour_over_zeta.erase(surface.zeta_address)) {
        if (const auto colour = g_targets.find(surface.zeta_address); colour != g_targets.end()) {
            colour_to_depth(colour->second, *depth, surface.zeta_address);
        }
    }
    // Ambient occlusion between the 3D scene and what is drawn over it (the HUD keeps depth testing on, with
    // ALWAYS and no writes, so "3D" means comparing or writing depth).
    constexpr uint32_t kDepthAlways = 0x207;
    const bool scene_draw = (state.depth_test && state.depth_function != kDepthAlways) || state.depth_write;
    if (surface.zeta_address && target->width >= 640) {
        SceneState& scene = g_scenes[surface.zeta_address];
        if (scene_draw) {
            scene.drawn = true;
        } else if (scene.drawn && !scene.occluded) {
            scene.occluded = true;
            if (DepthTarget* scene_depth = get_depth_target(surface.zeta_address, target->width, target->height)) {
                apply_occlusion(*target, *scene_depth);
                ++g_occlusion_scenes;
            }
            if (g_shown_surfaces.count(surface.color_address)) apply_scene_effects(*target, scene, surface.color_address);
        }
    }
    draw = PreparedDraw{target, depth, shader};
    return true;
}

// A copy of a render target's colour, for a draw that samples the target it draws into.
ID3D11ShaderResourceView* target_copy(const Target& target) {
    static ComPtr<ID3D11Texture2D> copy;
    static ComPtr<ID3D11ShaderResourceView> copy_view;
    D3D11_TEXTURE2D_DESC desc{}, existing{};
    target.texture->GetDesc(&desc);
    if (copy) copy->GetDesc(&existing);
    if (!copy || existing.Width != desc.Width || existing.Height != desc.Height || existing.Format != desc.Format ||
        existing.MipLevels != desc.MipLevels) {
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;
        copy.Reset();
        copy_view.Reset();
        if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &copy); FAILED(hr)) {
            log_resource_failure("a copy of the render target", hr, desc);
            return nullptr;
        }
        g_device->CreateShaderResourceView(copy.Get(), nullptr, &copy_view);
    }
    g_context->CopyResource(copy.Get(), target.texture.Get());
    return copy_view.Get();
}

// Render target, viewport, scissor, raster, blend, depth state, combiner constants, textures and samplers.
void bind_draw_state(const SurfaceState& surface, const DrawState& state, const PreparedDraw& draw) {
    Target* target = draw.target;
    // A depth buffer sampled as a texture: copied as colour first (this binds its own state, so before ours).
    for (const TextureStage& stage : state.textures) {
        const uint32_t address = stage.descriptor.address;  // a set lookup: most draws sample no depth buffer
        if (stage.enabled && g_zeta_addresses.count(address) && !g_targets.count(address)) refresh_depth_colour(address);
    }
    const uint32_t texture = state.textures[0].enabled ? state.textures[0].descriptor.address : 0;
    const RendererNarrowing hud = renderer_game_narrowing(surface, state.screen_left, state.screen_right,
                                                          state.screen_top, state.screen_bottom, texture);
    if (g_surface_scale_width != target->width || g_surface_scale_height != target->height ||
        g_surface_scale_pixels_x != surface.pixels_x || g_surface_scale_pixels_y != surface.pixels_y ||
        g_surface_scale_narrow != hud.factor || g_surface_scale_anchor != hud.anchor ||
        g_surface_scale_narrow_y != hud.factor_y || g_surface_scale_anchor_y != hud.anchor_y) {
        const float scale[8] = {2.0f * surface.pixels_x / target->width, 2.0f * surface.pixels_y / target->height,
                                hud.factor, hud.anchor, hud.factor_y, hud.anchor_y, 0, 0};
        g_context->UpdateSubresource(g_surface_constants.Get(), 0, nullptr, scale, 0, 0);
        g_surface_scale_width = target->width;
        g_surface_scale_height = target->height;
        g_surface_scale_pixels_x = surface.pixels_x;
        g_surface_scale_pixels_y = surface.pixels_y;
        g_surface_scale_narrow = hud.factor;
        g_surface_scale_anchor = hud.anchor;
        g_surface_scale_narrow_y = hud.factor_y;
        g_surface_scale_anchor_y = hud.anchor_y;
    }
    BoundState& bound = g_bound;
    if (!bound.valid) {
        bound = BoundState{};
        ID3D11Buffer* surface_constants = g_surface_constants.Get();
        g_context->VSSetConstantBuffers(0, 1, &surface_constants);
        ID3D11Buffer* constants = g_constants.Get();
        g_context->PSSetConstantBuffers(0, 1, &constants);
    }

    ID3D11RenderTargetView* rtv = target->rtv.Get();
    ID3D11DepthStencilView* dsv = draw.depth ? draw.depth->dsv.Get() : nullptr;
    if (!bound.valid || rtv != bound.rtv || dsv != bound.dsv) {
        // A surface can be both the target and a texture of the same draw; unbind it from the pixel shader first.
        ID3D11ShaderResourceView* none[4] = {};
        g_context->PSSetShaderResources(0, 4, none);
        std::fill(std::begin(bound.views), std::end(bound.views), nullptr);
        g_context->OMSetRenderTargets(1, &rtv, dsv);
        bound.rtv = rtv, bound.dsv = dsv;
    }
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(target->width), scaled_float(target->height), 0, 1};
    if (!bound.valid || std::memcmp(&viewport, &bound.viewport, sizeof(viewport)) != 0) {
        g_context->RSSetViewports(1, &viewport);
        bound.viewport = viewport;
    }
    const auto clip_x0 = static_cast<uint32_t>(std::lround(hud.apply(static_cast<float>(surface.clip_x))));
    const auto clip_x1 =
        static_cast<uint32_t>(std::lround(hud.apply(static_cast<float>(surface.clip_x + surface.clip_width))));
    const auto clip_y0 = static_cast<uint32_t>(std::lround(hud.apply_y(static_cast<float>(surface.clip_y))));
    const auto clip_y1 =
        static_cast<uint32_t>(std::lround(hud.apply_y(static_cast<float>(surface.clip_y + surface.clip_height))));
    const D3D11_RECT scissor{scaled(clip_x0), scaled(clip_y0), scaled(clip_x1), scaled(clip_y1)};
    if (!bound.valid || std::memcmp(&scissor, &bound.scissor, sizeof(scissor)) != 0) {
        g_context->RSSetScissorRects(1, &scissor);
        bound.scissor = scissor;
    }
    if (ID3D11RasterizerState* rasterizer = rasterizer_for(state); !bound.valid || rasterizer != bound.rasterizer) {
        g_context->RSSetState(rasterizer);
        bound.rasterizer = rasterizer;
    }
    // Direct3D has one blend factor for colour and alpha: CONSTANT_ALPHA (the weights Spider-Man 2 adds its clouds'
    // layers with) is that factor with the alpha in every channel. A draw mixing it with CONSTANT_COLOR cannot be
    // expressed.
    const auto constant_alpha = [](uint32_t gl) { return gl == 0x8003 || gl == 0x8004; };
    float blend_color[4];
    std::memcpy(blend_color, state.blend_color, sizeof(blend_color));
    if (signed_source(state)) std::fill(std::begin(blend_color), std::end(blend_color), 1.0f / 255);
    if (constant_alpha(state.blend_source) || constant_alpha(state.blend_destination)) {
        blend_color[0] = blend_color[1] = blend_color[2] = blend_color[3];
    }
    if (ID3D11BlendState* blend = blend_state(state);
        !bound.valid || blend != bound.blend || std::memcmp(blend_color, bound.blend_color, sizeof(bound.blend_color)) != 0) {
        g_context->OMSetBlendState(blend, blend_color, 0xFFFFFFFF);
        bound.blend = blend;
        std::memcpy(bound.blend_color, blend_color, sizeof(bound.blend_color));
    }
    if (ID3D11DepthStencilState* depth = depth_state(state);
        !bound.valid || depth != bound.depth || state.stencil_reference != bound.stencil_reference) {
        g_context->OMSetDepthStencilState(depth, state.stencil_reference);
        bound.depth = depth;
        bound.stencil_reference = state.stencil_reference;
    }

    // A linear texture is addressed in texels; when it is one of our render targets, its real width (from the
    // surface pitch) can be larger than the texture's rectangle, so normalise by the render target's size.
    CombinerConstants constants_data = state.constants;
    for (int i = 0; i < 4; ++i) {
        if (!state.textures[i].enabled || !texture_is_linear(state.textures[i].descriptor)) continue;
        const uint32_t address = state.textures[i].descriptor.address;
        const Target* rendered = nullptr;
        if (const auto found = g_targets.find(address); found != g_targets.end()) {
            rendered = &found->second;
        } else if (const auto depth = g_depth_colours.find(address); depth != g_depth_colours.end()) {
            rendered = &depth->second;
        }
        if (!rendered) continue;
        constants_data.texture_scale[i][0] = 1.0f / rendered->width;
        constants_data.texture_scale[i][1] = 1.0f / rendered->height;
    }
    // z of texture_scale: the stage reads a depth buffer as colour (a dependent lookup after it fades over a longer
    // distance, see dependent_depth in nv2a_combiner.cpp).
    for (int i = 0; i < 4; ++i) {
        constants_data.texture_scale[i][2] =
            state.textures[i].enabled && g_depth_colours.count(state.textures[i].descriptor.address) ? 1.0f : 0.0f;
    }
    renderer_game_draw(surface, state, constants_data.alpha_reference);  // a game's own fades (MC3's blurs)
    if (!bound.valid || std::memcmp(&constants_data, &bound.constants, sizeof(constants_data)) != 0) {
        g_context->UpdateSubresource(g_constants.Get(), 0, nullptr, &constants_data, 0, 0);
        bound.constants = constants_data;
    }

    ID3D11ShaderResourceView* views[4] = {};
    ID3D11SamplerState* samplers[4] = {};
    refresh_write_tracking();
    for (int i = 0; i < 4; ++i) {
        if (!state.textures[i].enabled) continue;
        views[i] = texture_view(state.textures[i], state.screen_left <= state.screen_right);
        // A texture looked up with the previous stage's texel (Spider-Man 2's shadow fade: the depth's bytes pick a
        // place on a ramp) is read with a linear filter, so the fade climbs smoothly instead of in 24 visible steps.
        const uint32_t mode = (state.program.stage_program >> (5 * i)) & 0x1F;
        if (mode == 0xF || mode == 0x10) {
            TextureStage linear = state.textures[i];
            linear.filter = (linear.filter & 0xFFFF) | 0x02020000;  // magnification and minification linear, no mipmaps
            samplers[i] = sampler(linear);
        } else {
            samplers[i] = sampler(state.textures[i]);
        }
        // The draw reads its own target: the NV2A then sees what is there now (MC3's motion blur writes its mask
        // into the frame's alpha this way). Direct3D cannot read and write one texture, so it reads a copy.
        if (views[i] == target->srv.Get()) {
            views[i] = target_copy(*target);
            g_self_read_surface = surface.color_address;
        }
    }
    if (!bound.valid || std::memcmp(views, bound.views, sizeof(views)) != 0) {
        g_context->PSSetShaderResources(0, 4, views);
        std::memcpy(bound.views, views, sizeof(views));
    }
    if (!bound.valid || std::memcmp(samplers, bound.samplers, sizeof(samplers)) != 0) {
        g_context->PSSetSamplers(0, 4, samplers);
        std::memcpy(bound.samplers, samplers, sizeof(samplers));
    }
    if (!bound.valid || draw.shader != bound.shader) {
        g_context->PSSetShader(draw.shader, nullptr, 0);
        bound.shader = draw.shader;
    }
    bound.valid = true;
}

// Appends `bytes` to a dynamic ring buffer: written without waiting (NO_OVERWRITE), renamed (DISCARD) only when
// full. Returns the byte offset, or ~0u on failure. The buffer grows (and is recreated) when too small.
struct RingBuffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;  // raw (byte address) view, for buffers the vertex shader reads
    uint32_t capacity = 0, used = 0;
};

constexpr uint32_t kMinimumRing = 8u << 20;  // vertex ring buffers start at this size

uint32_t ring_append(RingBuffer& ring, const void* data, uint32_t bytes, UINT bind, uint32_t element) {
    if (bytes > ring.capacity) {
        ring.capacity = std::max({bytes, ring.capacity * 2, kMinimumRing});
        ring.capacity -= ring.capacity % element;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = ring.capacity;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = bind;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (bind & D3D11_BIND_SHADER_RESOURCE) desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        ring.buffer.Reset();
        ring.view.Reset();
        if (const HRESULT hr = g_device->CreateBuffer(&desc, nullptr, &ring.buffer); FAILED(hr)) {
            log_resource_failure("a vertex ring buffer", hr, desc);
            ring.capacity = 0;  // tried again by the next append, never mapped while missing
            return ~0u;
        }
        if (bind & D3D11_BIND_SHADER_RESOURCE) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            view.BufferEx.NumElements = ring.capacity / 4;
            view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (FAILED(g_device->CreateShaderResourceView(ring.buffer.Get(), &view, &ring.view))) {
                ring.capacity = 0;
                return ~0u;
            }
        }
        ring.used = ring.capacity;  // forces a DISCARD below
    }
    ring.used = (ring.used + element - 1) / element * element;
    const bool wrap = ring.used + bytes > ring.capacity;
    if (wrap) ring.used = 0;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(ring.buffer.Get(), 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                              &mapped))) {
        return ~0u;
    }
    std::memcpy(static_cast<uint8_t*>(mapped.pData) + ring.used, data, bytes);
    g_context->Unmap(ring.buffer.Get(), 0);
    const uint32_t offset = ring.used;
    ring.used += bytes;
    return offset;
}

RingBuffer g_attribute_ring, g_index_ring;
FixedFunctionConstants g_fixed_function_uploaded{};
bool g_fixed_function_valid = false;

// Vertex programs translated to HLSL, by VertexProgram key. Compiled on helper threads, and kept on disk so that
// later runs find them at once; until ready (or if they do not compile) the CPU runs the program.
struct ProgramShader {
    ComPtr<ID3D11VertexShader> shader;
    enum { kPending, kReady, kFailed } state = kPending;
};
std::mutex g_program_mutex;
std::unordered_map<uint64_t, ProgramShader> g_program_shaders;  // under g_program_mutex

ProgramConstants g_program_uploaded{};
bool g_program_valid = false;

// A draw whose vertex shader reads the guest's vertex bytes (kVertexDataShader): uploads them and the indices,
// and the shader's constants (b1) when they differ from the last ones uploaded into `buffer`.
template <typename Constants>
void draw_vertex_data(const SurfaceState& surface, const DrawState& state, ID3D11VertexShader* shader,
                      ID3D11Buffer* buffer, const Constants& constants, Constants& uploaded, bool& valid,
                      const void* vertex_data, uint32_t vertex_bytes, const uint32_t* indices, uint32_t index_count) {
    if (!index_count || !shader) return;
    const uint64_t draw_start = ticks();
    PreparedDraw draw;
    if (!prepare_draw(surface, state, draw)) return;
    uint64_t mark = ticks();
    g_stats.part_ticks[0] += mark - draw_start;
    uint32_t first = 0;
    if (vertex_bytes) {
        first = ring_append(g_attribute_ring, vertex_data, vertex_bytes, D3D11_BIND_SHADER_RESOURCE, 4);
        if (first == ~0u) return;
    }
    const uint32_t index_offset = ring_append(g_index_ring, indices, index_count * 4,
                                              D3D11_BIND_INDEX_BUFFER, 4);
    if (index_offset == ~0u) return;
    // The transform state changes far less often than draws: uploaded only when different.
    if (!valid || std::memcmp(&constants, &uploaded, sizeof(constants)) != 0) {
        g_context->UpdateSubresource(buffer, 0, nullptr, &constants, 0, 0);
        uploaded = constants;
        valid = true;
    }
    const uint32_t draw_constants[4] = {first, 0, 0, 0};
    g_context->UpdateSubresource(g_fixed_function_draw_constants.Get(), 0, nullptr, draw_constants, 0, 0);
    const uint64_t uploaded_at = ticks();
    g_stats.part_ticks[1] += uploaded_at - mark;
    bind_draw_state(surface, state, draw);
    mark = ticks();
    g_stats.part_ticks[3] += mark - uploaded_at;
    // Six calls that say the same from one draw to the next: only what differs is set, as bind_draw_state does.
    BoundState& bound = g_bound;
    if (!bound.vertex_data) {
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bound.vertex_shader = nullptr, bound.vertex_constants = nullptr, bound.vertex_view = nullptr;
        bound.index_buffer = nullptr;
        bound.vertex_data = true;
    }
    if (buffer != bound.vertex_constants) {
        ID3D11Buffer* buffers[2] = {buffer, g_fixed_function_draw_constants.Get()};
        g_context->VSSetConstantBuffers(1, 2, buffers);
        bound.vertex_constants = buffer;
    }
    if (ID3D11ShaderResourceView* view = g_attribute_ring.view.Get(); view != bound.vertex_view) {
        g_context->VSSetShaderResources(0, 1, &view);
        bound.vertex_view = view;
    }
    if (ID3D11Buffer* index_buffer = g_index_ring.buffer.Get(); index_buffer != bound.index_buffer) {
        g_context->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R32_UINT, 0);
        bound.index_buffer = index_buffer;
    }
    if (shader != bound.vertex_shader) {
        g_context->VSSetShader(shader, nullptr, 0);
        bound.vertex_shader = shader;
    }
    g_context->DrawIndexed(index_count, index_offset / 4, 0);
    const uint64_t end = ticks();
    g_stats.part_ticks[4] += end - mark;
    g_stats.draw_ticks += end - draw_start;
}

}  // namespace

bool renderer_load_picture(const std::filesystem::path& file, std::vector<uint8_t>& bgra, UINT& width, UINT& height) {
    return load_picture(file, bgra, width, height);
}

bool renderer_has_fixed_function() { return g_fixed_function_vertex_shader != nullptr; }

namespace {

// --- the render thread ---
//
// One thread decoded the game's drawing commands and made Direct3D's calls for them, and it is the port's limit: at
// 120 fps, and in split screen. The two halves are now two threads. The thread that decodes (the GPU thread) copies
// each draw (state, constants, vertex bytes, indices) and each clear into a queue; the render thread takes them in
// order and does what the first did: finds the textures, binds the state, draws.
//
// Everything else the renderer does (presenting, blits, the rare draw with CPU vertices, debugging reads) happens on
// the calling thread once the queue is empty (flush_queue), so that the two never touch Direct3D or the renderer's
// tables at the same time, and no lock is needed around them.
//
// The game learns how far the GPU has come from the push buffer's GET and from the methods that answer it (reference,
// semaphore, flip, traps): it changes a texture or reuses memory only after the draws that use them. So the port
// publishes GET as the render thread finishes (renderer_progress, the value each queued command carries), and waits
// for the queue before those methods (renderer_flush): for the game the GPU is as far as what is really drawn.
//
// The queue is a ring of bytes written by one thread and read by one: records of whole 16 bytes, never split across
// the end (a skip record fills what is left). Setting render_thread 0 turns it off: everything on the GPU thread again.
enum class Queued : uint32_t { kSkip, kMark, kCall, kClear, kFixedFunction, kProgram };

struct QueuedHeader {
    uint32_t bytes;  // of the whole record
    Queued kind;
    uint32_t progress;
    uint32_t has_progress;
};

struct QueuedCall {
    void (*call)(uint32_t, uint32_t);
    uint32_t first, second;
};

struct QueuedClear {
    SurfaceState surface;
    uint32_t flags, color, depth_stencil;
    int x0, y0, x1, y1;
};

template <typename Constants>
struct QueuedDraw {
    Constants constants;  // first: aligned to 16 bytes like the record
    SurfaceState surface;
    DrawState state;
    uint64_t key;  // the vertex program's (kProgram)
    uint32_t vertex_bytes, index_count;
};

// A power of two, small enough to stay in the processor's cache: the two threads go round the same few megabytes.
// At 64 MB every draw was written to and read from memory neither had in its cache.
constexpr uint32_t kQueueBytes = 4u << 20;
uint8_t* g_queue = nullptr;
std::atomic<uint32_t> g_queue_head{0}, g_queue_tail{0};  // bytes written and read, counted without wrapping
std::atomic<bool> g_render_waiting{false}, g_front_waiting{false};
void (*g_publish_progress)(uint32_t) = nullptr;
uint32_t g_progress = 0;
bool g_has_progress = false;

void wait_for_change(std::atomic<uint32_t>& value, uint32_t seen, std::atomic<bool>& waiting) {
    for (int spin = 0; spin < 4000; ++spin) {  // the other thread is nearly always about to move it
        if (value.load(std::memory_order_acquire) != seen) return;
        _mm_pause();
    }
    waiting.store(true);
    if (value.load() == seen) WaitOnAddress(&value, &seen, sizeof(seen), 1);  // 1 ms: a lost wake costs no more
    waiting.store(false);
}

// Room for a record of `bytes` (whole 16s), in one piece: where to write it.
uint8_t* reserve_queued(uint32_t bytes) {
    for (;;) {
        const uint32_t head = g_queue_head.load(std::memory_order_relaxed);
        const uint32_t tail = g_queue_tail.load(std::memory_order_acquire);
        const uint32_t at = head & (kQueueBytes - 1), to_end = kQueueBytes - at;
        const uint32_t needed = to_end < bytes ? to_end + bytes : bytes;
        if (kQueueBytes - (head - tail) < needed) {
            const uint64_t start = ticks();
            wait_for_change(g_queue_tail, tail, g_front_waiting);
            g_queue_waits.full_ticks += ticks() - start;
            continue;
        }
        if (to_end < bytes) {
            *reinterpret_cast<QueuedHeader*>(g_queue + at) = {to_end, Queued::kSkip, 0, 0};
            g_queue_head.store(head + to_end, std::memory_order_release);
            continue;
        }
        return g_queue + at;
    }
}

void commit_queued(uint8_t* record, uint32_t bytes, Queued kind) {
    *reinterpret_cast<QueuedHeader*>(record) = {bytes, kind, g_progress, g_has_progress ? 1u : 0u};
    g_has_progress = false;
    g_queue_head.store(g_queue_head.load(std::memory_order_relaxed) + bytes, std::memory_order_release);
    if (g_render_waiting.load()) WakeByAddressSingle(&g_queue_head);
}

void note_queued_draw(const SurfaceState& surface, const DrawState& state) {
    if (state.cull_enable && state.cull_face == 0x408) return;  // as prepare_draw: nothing is drawn
    uint32_t& width = g_queued_widths[surface.color_address];
    width = std::max(width, surface.width);
    constexpr uint32_t kDepthAlways = 0x207;
    const bool scene_draw = (state.depth_test && state.depth_function != kDepthAlways) || state.depth_write;
    if (scene_draw && surface.zeta_address && width >= 640) g_queued_scenes[surface.zeta_address] = true;
}

template <typename Constants>
bool queue_draw(Queued kind, const SurfaceState& surface, const DrawState& state, uint64_t key,
                const Constants& constants, const void* vertex_data, uint32_t vertex_bytes,
                const std::vector<uint32_t>& indices) {
    const uint32_t vertex_room = (vertex_bytes + 3) & ~3u;
    const uint64_t bytes64 = (sizeof(QueuedHeader) + sizeof(QueuedDraw<Constants>) + uint64_t{vertex_room} +
                              indices.size() * 4 + 15) & ~uint64_t{15};
    if (bytes64 > kQueueBytes / 4) return false;  // a huge draw: done in its turn by the caller, not queued
    const uint32_t bytes = static_cast<uint32_t>(bytes64);
    uint8_t* record = reserve_queued(bytes);
    auto* draw = reinterpret_cast<QueuedDraw<Constants>*>(record + sizeof(QueuedHeader));
    std::memcpy(&draw->constants, &constants, sizeof(constants));
    draw->surface = surface;
    draw->state = state;
    draw->key = key;
    draw->vertex_bytes = vertex_bytes;
    draw->index_count = static_cast<uint32_t>(indices.size());
    uint8_t* data = reinterpret_cast<uint8_t*>(draw + 1);
    if (vertex_bytes) std::memcpy(data, vertex_data, vertex_bytes);
    std::memcpy(data + vertex_room, indices.data(), indices.size() * 4);
    note_queued_draw(surface, state);
    commit_queued(record, bytes, kind);
    return true;
}

void draw_fixed_function_now(const SurfaceState& surface, const DrawState& state, const FixedFunctionConstants& constants,
                             const void* vertex_data, uint32_t vertex_bytes, const uint32_t* indices,
                             uint32_t index_count) {
    draw_vertex_data(surface, state, g_fixed_function_vertex_shader.Get(), g_fixed_function_constants.Get(), constants,
                     g_fixed_function_uploaded, g_fixed_function_valid, vertex_data, vertex_bytes, indices, index_count);
}

void draw_program_now(const SurfaceState& surface, const DrawState& state, uint64_t key, const ProgramConstants& constants,
                      const void* vertex_data, uint32_t vertex_bytes, const uint32_t* indices, uint32_t index_count) {
    ID3D11VertexShader* shader = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_program_mutex);
        const auto found = g_program_shaders.find(key);
        if (found != g_program_shaders.end()) shader = found->second.shader.Get();
    }
    draw_vertex_data(surface, state, shader, g_program_constants.Get(), constants, g_program_uploaded, g_program_valid,
                     vertex_data, vertex_bytes, indices, index_count);
}

template <typename Constants>
void run_queued_draw(const QueuedHeader* header) {
    const auto* draw = reinterpret_cast<const QueuedDraw<Constants>*>(header + 1);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(draw + 1);
    const auto* indices = reinterpret_cast<const uint32_t*>(data + ((draw->vertex_bytes + 3) & ~3u));
    if constexpr (std::is_same_v<Constants, ProgramConstants>) {
        draw_program_now(draw->surface, draw->state, draw->key, draw->constants, data, draw->vertex_bytes, indices,
                         draw->index_count);
    } else {
        draw_fixed_function_now(draw->surface, draw->state, draw->constants, data, draw->vertex_bytes, indices,
                                draw->index_count);
    }
}

unsigned long g_render_cores = 0;  // set by start_render_thread

void render_thread() {
    if (const unsigned long cores = g_render_cores ? g_render_cores : gpu_cores()) {
        SetThreadAffinityMask(GetCurrentThread(), cores);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    profile_name_thread(false, "render");
    g_render_thread_id = GetCurrentThreadId();
    for (;;) {
        const uint32_t tail = g_queue_tail.load(std::memory_order_relaxed);
        if (g_queue_head.load(std::memory_order_acquire) == tail) {
            ProfileScope scope(ProfileTime::idle);
            wait_for_change(g_queue_head, tail, g_render_waiting);
            continue;
        }
        const auto* header = reinterpret_cast<const QueuedHeader*>(g_queue + (tail & (kQueueBytes - 1)));
        switch (header->kind) {
            case Queued::kClear: {
                const auto* clear = reinterpret_cast<const QueuedClear*>(header + 1);
                clear_now(clear->surface, clear->flags, clear->color, clear->depth_stencil, clear->x0, clear->y0,
                          clear->x1, clear->y1);
                break;
            }
            case Queued::kFixedFunction: run_queued_draw<FixedFunctionConstants>(header); break;
            case Queued::kProgram: run_queued_draw<ProgramConstants>(header); break;
            case Queued::kCall: {
                const auto* call = reinterpret_cast<const QueuedCall*>(header + 1);
                call->call(call->first, call->second);
                break;
            }
            default: break;  // kSkip, kMark
        }
        if (header->has_progress && g_publish_progress) g_publish_progress(header->progress);
        g_queue_tail.store(tail + header->bytes, std::memory_order_release);
        if (g_front_waiting.load()) WakeByAddressAll(&g_queue_tail);
    }
}

void flush_queue() {
    if (!g_queue_on || GetCurrentThreadId() == g_render_thread_id) return;
    const uint32_t head = g_queue_head.load(std::memory_order_relaxed);
    ++g_queue_waits.flushes;
    if (g_queue_tail.load(std::memory_order_acquire) != head) {
        ++g_queue_waits.waited_flushes;
        const uint64_t start = ticks();
        for (;;) {
            const uint32_t tail = g_queue_tail.load(std::memory_order_acquire);
            if (tail == head) break;
            wait_for_change(g_queue_tail, tail, g_front_waiting);
        }
        g_queue_waits.flush_ticks += ticks() - start;
    }
    // Progress noted while draws were queued, with no command after it to carry it: the caller is about to wait for
    // the game, which may itself be waiting for that progress.
    if (g_has_progress) {
        g_has_progress = false;
        if (g_publish_progress) g_publish_progress(g_progress);
    }
}

void start_render_thread() {
    // Setting render_thread: 1 = on (the default), 0 = everything on the GPU thread as before, 2 = taking turns (see
    // renderer_present). With fewer than three processors beside the game's the two threads would share one.
    const float wanted = setting_at_start("render_thread");
    unsigned long cores = emulation_cores(), count = 0;
    for (; cores; cores &= cores - 1) ++count;
    if (!emulation_cores()) count = std::thread::hardware_concurrency();  // the game is not kept on one core
    if (wanted == 0 || count < 3) {
        log_line("renderer: no render thread (%s), draws on the GPU thread",
                 wanted == 0 ? "setting render_thread 0" : "too few processors");
        return;
    }
    g_queue = static_cast<uint8_t*>(VirtualAlloc(nullptr, kQueueBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_queue) {
        log_line("renderer: no memory for the draw queue, draws on the GPU thread");
        return;
    }
    g_queue_alternates = wanted == 2;
    // Called on the GPU thread: it and the render thread each on cores of their own.
    const GpuCoreSplit split = gpu_core_split();
    if (split.render) {
        SetThreadAffinityMask(GetCurrentThread(), split.gpu);
        g_render_cores = split.render;
    }
    std::thread(render_thread).detach();
    while (!g_render_thread_id) Sleep(0);
    g_queue_on = true;
    log_line("renderer: draws and clears on a render thread (id %lu, mask %08lX), the GPU thread (id %lu) on mask %08lX",
             g_render_thread_id, split.render ? split.render : gpu_cores(), GetCurrentThreadId(),
             split.gpu ? split.gpu : gpu_cores());
}

}  // namespace

void renderer_set_progress(void (*publish)(uint32_t)) { g_publish_progress = publish; }

bool renderer_idle() {
    return !g_queue_on || g_queue_tail.load(std::memory_order_acquire) == g_queue_head.load(std::memory_order_relaxed);
}

// With nothing queued the progress is published at once; else the next command queued carries the latest value (one
// kept back and then published by a later command would take the game's view of the GPU backwards).
void renderer_progress(uint32_t value) {
    if (renderer_idle()) {
        g_has_progress = false;
        if (g_publish_progress) g_publish_progress(value);
    } else {
        g_progress = value, g_has_progress = true;
    }
}

void renderer_mark() {
    if (!g_has_progress) return;
    if (renderer_idle()) return renderer_progress(g_progress);
    uint8_t* record = reserve_queued(sizeof(QueuedHeader));
    commit_queued(record, sizeof(QueuedHeader), Queued::kMark);
}

void renderer_flush() { flush_queue(); }

void renderer_after_drawing(void (*call)(uint32_t, uint32_t), uint32_t first, uint32_t second) {
    if (renderer_idle()) return call(first, second);
    constexpr uint32_t kBytes = (sizeof(QueuedHeader) + sizeof(QueuedCall) + 15) & ~15u;
    uint8_t* record = reserve_queued(kBytes);
    *reinterpret_cast<QueuedCall*>(record + sizeof(QueuedHeader)) = {call, first, second};
    commit_queued(record, kBytes, Queued::kCall);
}

void renderer_clear(const SurfaceState& surface, uint32_t flags, uint32_t color, uint32_t depth_stencil, int x0, int y0,
                    int x1, int y1) {
    if ((flags & 0x1) && surface.zeta_address && surface.width >= 640) g_queued_scenes[surface.zeta_address] = false;
    if (!g_queue_on) return clear_now(surface, flags, color, depth_stencil, x0, y0, x1, y1);
    constexpr uint32_t kBytes = (sizeof(QueuedHeader) + sizeof(QueuedClear) + 15) & ~15u;
    uint8_t* record = reserve_queued(kBytes);
    *reinterpret_cast<QueuedClear*>(record + sizeof(QueuedHeader)) = {surface, flags, color, depth_stencil, x0, y0, x1, y1};
    commit_queued(record, kBytes, Queued::kClear);
}

void renderer_draw_fixed_function(const SurfaceState& surface, const DrawState& state,
                                  const FixedFunctionConstants& constants, const void* vertex_data,
                                  uint32_t vertex_bytes, const std::vector<uint32_t>& indices) {
    if (indices.empty()) return;
    if (g_queue_on && queue_draw(Queued::kFixedFunction, surface, state, 0, constants, vertex_data, vertex_bytes, indices)) {
        return;
    }
    flush_queue();
    if (g_render_thread_id) note_queued_draw(surface, state);
    draw_fixed_function_now(surface, state, constants, vertex_data, vertex_bytes, indices.data(),
                            static_cast<uint32_t>(indices.size()));
}

void renderer_draw_program(const SurfaceState& surface, const DrawState& state, uint64_t key,
                           const ProgramConstants& constants, const void* vertex_data, uint32_t vertex_bytes,
                           const std::vector<uint32_t>& indices) {
    if (indices.empty()) return;
    if (g_queue_on && queue_draw(Queued::kProgram, surface, state, key, constants, vertex_data, vertex_bytes, indices)) {
        return;
    }
    flush_queue();
    if (g_render_thread_id) note_queued_draw(surface, state);
    draw_program_now(surface, state, key, constants, vertex_data, vertex_bytes, indices.data(),
                     static_cast<uint32_t>(indices.size()));
}

bool renderer_prepare_program(uint64_t key, const std::function<std::string()>& source) {
    if (!g_fixed_function_vertex_shader || !g_program_constants) return false;  // no GPU vertex path at all
    {
        std::lock_guard<std::mutex> lock(g_program_mutex);
        const auto found = g_program_shaders.find(key);
        if (found != g_program_shaders.end()) return found->second.state == ProgramShader::kReady;
        g_program_shaders[key].state = ProgramShader::kPending;
    }
    const std::string text = std::string(kVertexDataShader) + kVertexProgramShader + source();
    const std::filesystem::path file = shader_cache_file("vp", text);
    const auto finish = [key](ComPtr<ID3D11VertexShader> shader) {
        std::lock_guard<std::mutex> lock(g_program_mutex);
        ProgramShader& entry = g_program_shaders[key];
        entry.shader = shader;
        entry.state = shader ? ProgramShader::kReady : ProgramShader::kFailed;
    };

    if (ComPtr<ID3D11VertexShader> ready = take_preloaded(g_preloaded_vertex, file)) {
        finish(ready);
        return true;
    }
    const std::vector<char> code = read_cached_shader(file);
    ComPtr<ID3D11VertexShader> shader;
    if (!code.empty() && SUCCEEDED(g_device->CreateVertexShader(code.data(), code.size(), nullptr, &shader))) {
        finish(shader);
        return true;
    }
    // Compiling takes a few hundred milliseconds: on a helper thread, while the CPU draws with this program.
    std::thread([key, text, file, finish] {
        char name[48];
        std::snprintf(name, sizeof(name), "vertex program %016llX", static_cast<unsigned long long>(key));
        const uint64_t start = ticks();
        ComPtr<ID3D11VertexShader> shader;
        if (ComPtr<ID3DBlob> code = compile(text.data(), text.size(), "vs_5_0", name)) {
            if (SUCCEEDED(g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                       &shader))) {
                std::error_code error;
                std::filesystem::create_directories(file.parent_path(), error);
                std::ofstream(file, std::ios::binary)
                    .write(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
            }
        }
        log_line("renderer: %s %s in %.1f ms", name, shader ? "compiled" : "stays on the CPU",
                 milliseconds(ticks() - start));
        if (!shader) log_line("%s", text.c_str() + std::strlen(kVertexDataShader) + std::strlen(kVertexProgramShader));
        finish(shader);
    }).detach();
    return false;
}

void renderer_draw(const SurfaceState& surface, const DrawState& state, const std::vector<RenderVertex>& triangles) {
    if (triangles.empty()) return;
    flush_queue();  // a draw with its vertices worked out on the CPU: rare, done here in its turn
    const uint64_t draw_start = ticks();
    struct Account {
        uint64_t start;
        ~Account() { g_stats.draw_ticks += ticks() - start; }
    } account{draw_start};
    PreparedDraw draw;
    if (!prepare_draw(surface, state, draw)) return;
    uint64_t mark = ticks();
    const auto part = [&mark](int index) {
        const uint64_t now = ticks();
        g_stats.part_ticks[index] += now - mark;
        mark = now;
    };
    g_stats.part_ticks[0] += mark - draw_start;

    // Vertices go into a ring buffer: appended without waiting (NO_OVERWRITE), and the buffer is renamed (DISCARD)
    // only when it is full, instead of once per draw.
    const uint32_t bytes = static_cast<uint32_t>(triangles.size() * sizeof(RenderVertex));
    if (bytes > g_vertex_capacity) {
        g_vertex_capacity = std::max({bytes, g_vertex_capacity * 2, kMinimumRing});
        g_vertex_capacity -= g_vertex_capacity % sizeof(RenderVertex);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = g_vertex_capacity;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_vertex_buffer.Reset();
        if (const HRESULT hr = g_device->CreateBuffer(&desc, nullptr, &g_vertex_buffer); FAILED(hr)) {
            log_resource_failure("the vertex buffer", hr, desc);
            g_vertex_capacity = 0;  // the draw is skipped and the next one tries again
            return;
        }
        g_vertex_used = g_vertex_capacity;  // forces a DISCARD below
    }
    const bool wrap = g_vertex_used + bytes > g_vertex_capacity;
    if (wrap) g_vertex_used = 0;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(g_vertex_buffer.Get(), 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                              &mapped))) {
        return;
    }
    // One sequential copy: the buffer is write-combined memory, slow to write piecewise. The vertex shader maps
    // pixels to clip space over the render target's real size.
    const UINT first_vertex = g_vertex_used / sizeof(RenderVertex);
    std::memcpy(static_cast<uint8_t*>(mapped.pData) + g_vertex_used, triangles.data(), bytes);
    g_context->Unmap(g_vertex_buffer.Get(), 0);
    g_vertex_used += bytes;
    part(1);
    bind_draw_state(surface, state, draw);
    g_bound.vertex_data = false;  // this draw's own vertex buffer, layout and shader, below
    part(3);

    const UINT stride = sizeof(RenderVertex), offset = 0;
    ID3D11Buffer* buffer = g_vertex_buffer.Get();
    g_context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    g_context->IASetInputLayout(g_input_layout.Get());
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vertex_shader.Get(), nullptr, 0);
    g_context->Draw(static_cast<UINT>(triangles.size()), first_vertex);
    part(4);
}

void renderer_dump_target(uint32_t address, const char* prefix) {
    flush_queue();
    const auto found = g_targets.find(address);
    if (found != g_targets.end()) dump_frame(found->second.texture.Get(), prefix);
}

bool renderer_probe(uint32_t address, uint32_t x, uint32_t y, uint8_t bgra[4]) {
    flush_queue();
    const auto found = g_targets.find(address);
    if (found == g_targets.end() || x >= found->second.width || y >= found->second.height) return false;
    static ComPtr<ID3D11Texture2D> staging;
    if (!staging) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 1;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &staging); FAILED(hr)) {
            log_resource_failure("a staging copy", hr, desc);
            return false;
        }
    }
    const D3D11_BOX box{scaled_size(x), scaled_size(y), 0, scaled_size(x) + 1, scaled_size(y) + 1, 1};
    g_context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, found->second.texture.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    std::memcpy(bgra, mapped.pData, 4);
    g_context->Unmap(staging.Get(), 0);
    return true;
}

void renderer_note_vertex_work(uint64_t elapsed, uint32_t vertices) {
    g_stats.vertex_ticks += elapsed;
    g_stats.vertices += vertices;
}

void renderer_blit(const BlitRequest& b) {
    if (!g_device || !b.width || !b.height) return;
    flush_queue();  // it reads and writes the game's memory and the targets
    const auto source = g_targets.find(b.source);
    if (source != g_targets.end() && b.bytes_per_pixel == 4) {
        const Target& from = source->second;
        if (b.source_x + b.width > from.width || b.source_y + b.height > from.height) return;
        const auto existing = g_targets.find(b.destination);
        const uint32_t width = existing != g_targets.end() ? existing->second.width : b.destination_pitch / 4;
        const uint32_t height = existing != g_targets.end() ? existing->second.height : from.height;
        Target* to = get_target(b.destination, width, height);
        if (!to || b.destination_x + b.width > to->width || b.destination_y + b.height > to->height) return;
        // With a scale that is not whole, the rounded source and destination may differ by a pixel: the copy stays
        // inside the destination texture.
        const uint32_t to_x = scaled_size(b.destination_x), to_y = scaled_size(b.destination_y);
        const uint32_t from_x = scaled_size(b.source_x), from_y = scaled_size(b.source_y);
        const uint32_t copy_width = std::min(scaled_size(b.source_x + b.width) - from_x, scaled_size(to->width) - to_x);
        const uint32_t copy_height =
            std::min(scaled_size(b.source_y + b.height) - from_y, scaled_size(to->height) - to_y);
        const D3D11_BOX box{from_x, from_y, 0, from_x + copy_width, from_y + copy_height, 1};
        g_context->CopySubresourceRegion(to->texture.Get(), 0, to_x, to_y, 0, from.texture.Get(), 0, &box);
        return;
    }
    // Guest memory to guest memory.
    const uint32_t row = b.width * b.bytes_per_pixel;
    for (uint32_t y = 0; y < b.height; ++y) {
        const uint32_t from = b.source + (b.source_y + y) * b.source_pitch + b.source_x * b.bytes_per_pixel;
        const uint32_t to = b.destination + (b.destination_y + y) * b.destination_pitch + b.destination_x * b.bytes_per_pixel;
        uint8_t* source_row = physical_pointer(from);
        uint8_t* destination_row = physical_pointer(to);
        if (!source_row || !destination_row || !physical_pointer(from + row - 1) || !physical_pointer(to + row - 1)) return;
        std::memmove(destination_row, source_row, row);
    }
    const auto destination = g_targets.find(b.destination);
    if (destination != g_targets.end() && b.bytes_per_pixel == 4) {
        Target& to = destination->second;
        if (b.destination_x + b.width > to.width || b.destination_y + b.height > to.height) return;
        const uint8_t* pixels =
            physical_pointer(b.destination + b.destination_y * b.destination_pitch + b.destination_x * 4);
        if (g_scale == 1) {
            const D3D11_BOX box{b.destination_x, b.destination_y, 0, b.destination_x + b.width,
                                b.destination_y + b.height, 1};
            g_context->UpdateSubresource(to.texture.Get(), 0, &box, pixels, b.destination_pitch, 0);
        } else {
            upload_scaled(to, b.destination_x, b.destination_y, b.width, b.height, pixels, b.destination_pitch);
        }
    }
}

float renderer_aspect() {
    static const float aspect = [] {
        const float widescreen = setting_at_start("widescreen");
        if (widescreen == 0) return 4.0f / 3.0f;
        float shape = 16.0f / 9.0f;
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        // shortcut: the primary screen's shape, read once; a game window on a wider second screen keeps it.
        if (widescreen >= 2 && GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) {
            const float width = static_cast<float>(info.rcMonitor.right - info.rcMonitor.left);
            const float height = static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top);
            if (height > 0) shape = std::clamp(width / height, shape, 4.0f);
        }
        // <GAME>_SCREEN_SHAPE=<width/height>: that screen shape instead (testing 21:9 without such a screen).
        if (const char* forced = std::getenv(XPORT_ENV("SCREEN_SHAPE")); forced && widescreen >= 2) {
            shape = std::clamp(static_cast<float>(std::atof(forced)), 16.0f / 9.0f, 4.0f);
        }
        return shape;
    }();
    return aspect;
}

void renderer_note_scene() { g_frame_scene = true; }

bool renderer_scene_noted() { return g_frame_scene; }

ShownPicture renderer_shown_picture() { return g_shown_picture; }

bool renderer_surface_shown(uint32_t address) { return g_shown_surfaces.count(address) != 0; }

uint32_t renderer_self_read_surface() { return g_self_read_surface; }

uint64_t renderer_texture_hash(const uint8_t* data, size_t size) { return hash_memory(data, size); }

void renderer_toggle_effects() { renderer_set_effects(!g_effects_enabled); }

void renderer_set_effects(bool on) {
    g_effects_enabled = on;
    log_line("renderer: modern effects %s", on ? "on" : "off");
}

void renderer_set_overlay(RendererOverlay overlay) { g_overlay = overlay; }

void renderer_present(uint32_t address, uint32_t width, uint32_t height, uint32_t pitch) {
    flush_queue();  // the frame is whole before it is shown
    // Setting render_thread 2: the two ways take turns every 4 s, with the draws and frames of each in the log. On a PC
    // whose speed swings from one run to the next it is the comparison that can be read: the same scene, the same moment.
    if (g_queue_alternates) {
        static ULONGLONG since = GetTickCount64();
        static uint64_t draws_then = 0, frames = 0;
        ++frames;
        if (GetTickCount64() - since >= 4000) {
            const double seconds = (GetTickCount64() - since) / 1000.0;
            log_line("renderer: render thread %s: %.0f draws and %.1f frames a second", g_queue_on ? "on" : "off",
                     (g_total_draws - draws_then) / seconds, frames / seconds);
            g_queue_on = !g_queue_on, since = GetTickCount64(), draws_then = g_total_draws, frames = 0;
        }
    }
    const ForgetBound forget;  // presenting draws effects and the overlay (ImGui) with their own state
    if (!g_device || !width || !height) return;
    ID3D11Texture2D* frame = nullptr;
    ID3D11ShaderResourceView* frame_view = nullptr;
    const auto target = g_targets.find(address);
    if (target != g_targets.end()) {
        g_shown_surfaces.insert(address);
        g_picture_width = width, g_picture_height = height;
        // A film or loading screen is made for 16:9 at most: wider screens show it with bars at the sides.
        const float previous_aspect = g_shown_aspect;
        g_shown_aspect = g_frame_scene ? renderer_aspect() : std::min(renderer_aspect(), 16.0f / 9.0f);
        if (g_shown_aspect != previous_aspect) log_line("renderer: frames shown at %.3f", g_shown_aspect);
        g_frame_scene = false;
        const float aspect = g_shown_aspect;
        float window_width = static_cast<float>(g_swap_width), window_height = static_cast<float>(g_swap_height);
        if (window_width > window_height * aspect) {
            window_width = window_height * aspect;
        } else {
            window_height = window_width / aspect;
        }
        const float edge_x = static_cast<float>(width / kOverscan), edge_y = static_cast<float>(height / kOverscan);
        g_shown_picture = {edge_x,
                         edge_y,
                         static_cast<float>(width) - 2 * edge_x,
                         static_cast<float>(height) - 2 * edge_y,
                         std::floor((static_cast<float>(g_swap_width) - window_width) / 2),
                         std::floor((static_cast<float>(g_swap_height) - window_height) / 2),
                         window_width,
                         window_height,
                         width,
                         height};
        frame = target->second.texture.Get();
        frame_view = target->second.srv.Get();
        // Only the picture's part of a wider surface (widescreen: 720 of 768 pixels), copied out once per present.
        // Like a television, the outermost pixels are not shown either: games park things there that the TV's
        // overscan hid (MC3 slides its notification box off to the left and leaves its last 3 pixels on screen, and
        // when it is in, the box stops 13 pixels short of the edge, which a TV's overscan covered). 1/48 of each side,
        // the same share across and down so the picture keeps its shape.
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        const uint32_t full_width = std::min(scaled_size(width), desc.Width);
        const uint32_t full_height = std::min(scaled_size(height), desc.Height);
        const uint32_t crop_x = full_width / kOverscan, crop_y = full_height / kOverscan;
        const uint32_t visible_width = full_width - 2 * crop_x, visible_height = full_height - 2 * crop_y;
        {
            static ComPtr<ID3D11Texture2D> visible;
            static ComPtr<ID3D11ShaderResourceView> visible_view;
            D3D11_TEXTURE2D_DESC visible_desc{};
            if (visible) visible->GetDesc(&visible_desc);
            if (!visible || visible_desc.Width != visible_width || visible_desc.Height != visible_height ||
                visible_desc.Format != desc.Format) {
                visible_desc = desc;
                visible_desc.Width = visible_width;
                visible_desc.Height = visible_height;
                visible_desc.MipLevels = 1;
                visible_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                visible_desc.MiscFlags = 0;
                visible.Reset();
                visible_view.Reset();
                if (SUCCEEDED(g_device->CreateTexture2D(&visible_desc, nullptr, &visible))) {
                    g_device->CreateShaderResourceView(visible.Get(), nullptr, &visible_view);
                }
            }
            if (visible_view) {
                const D3D11_BOX box{crop_x, crop_y, 0, crop_x + visible_width, crop_y + visible_height, 1};
                g_context->CopySubresourceRegion(visible.Get(), 0, 0, 0, 0, frame, 0, &box);
                frame = visible.Get();
                frame_view = visible_view.Get();
            }
        }
    } else {
        // Nothing rendered there: show the guest memory (32-bit pixels).
        const uint8_t* pixels = physical_pointer(address);
        if (!pixels || !pitch || !physical_pointer(address + pitch * height - 1)) return;
        if (!g_upload || g_upload_width != width || g_upload_height != height) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            g_upload.Reset();
            g_upload_srv.Reset();
            g_device->CreateTexture2D(&desc, nullptr, &g_upload);
            g_device->CreateShaderResourceView(g_upload.Get(), nullptr, &g_upload_srv);
            g_upload_width = width;
            g_upload_height = height;
        }
        g_context->UpdateSubresource(g_upload.Get(), 0, nullptr, pixels, pitch, 0);
        frame = g_upload.Get();
        frame_view = g_upload_srv.Get();
    }
    if (g_presented % 600 == 599) {
        log_line("renderer: ambient occlusion on %u of %u scenes", g_occlusion_scenes, g_full_depth_clears);
        if (g_dlss.frames) {
            log_line("renderer: DLSS %s: waited %.2f ms a frame for the helper over %u frames", dlss_mode(),
                     g_dlss.waited_ms / g_dlss.frames, g_dlss.frames);
            g_dlss.waited_ms = 0, g_dlss.frames = 0;
        }
        g_occlusion_scenes = g_full_depth_clears = 0;
    }
    D3D11_TEXTURE2D_DESC frame_desc{};
    if (frame) frame->GetDesc(&frame_desc);
    ensure_swap_chain_size();
    const uint64_t effects_start = ticks();
    std::unique_lock<std::mutex> drawing(g_present_mutex);
    if (g_final_lock) g_final_lock->AcquireSync(0, INFINITE);
    present_frame(frame_view, frame_desc.Width, frame_desc.Height,
                  target != g_targets.end() && g_graded_surfaces.count(address) != 0);
    renderer_game_frame_shown();
    ++g_presented;
    g_compile_waited_ms = 0;
    ++g_stats.presents;
    forget_unused_textures();
    log_statistics();
    const uint64_t overlay_start = ticks();
    if (const RendererOverlay overlay = g_overlay.load(); overlay && g_final_rtv) {
        overlay(g_device.Get(), g_context.Get(), g_final_rtv.Get(), g_swap_width, g_swap_height);
    }
    const uint64_t overlay_end = ticks();
    // <GAME>_DUMP_SECONDS=<s>: a frame every s seconds of real time (frame-<milliseconds>.bmp... named by count).
    static const double dump_seconds = [] {
        char value[16] = {};
        return GetEnvironmentVariableA(XPORT_ENV("DUMP_SECONDS"), value, sizeof(value)) ? std::atof(value) : 0.0;
    }();
    static ULONGLONG next_timed_dump = 0;
    bool timed_dump = false;
    if (dump_seconds > 0) {
        const ULONGLONG now = GetTickCount64();
        if (!next_timed_dump) next_timed_dump = now;
        if (now >= next_timed_dump) {
            timed_dump = true;
            // From now: a save slower than the interval (a large picture) would otherwise make every frame due.
            next_timed_dump = now + static_cast<ULONGLONG>(dump_seconds * 1000);
        }
    }
    if ((g_dump_every && g_presented % g_dump_every == 0) || timed_dump) {
        dump_frame(frame);  // the game's frame at its own size, not the window
        static const bool dump_window = GetEnvironmentVariableA(XPORT_ENV("DUMP_WINDOW"), nullptr, 0) != 0;
        if (dump_window) dump_frame(g_final_texture.Get(), "window");  // with the effects and the overlay
        const auto extra = g_targets.find(g_dump_target);
        if (extra != g_targets.end()) dump_frame(extra->second.texture.Get(), "target");
    }
    if (g_final_lock) g_final_lock->ReleaseSync(0);
    if (g_present_threaded) {
        g_present_pending = true;
        drawing.unlock();
        g_present_wake.notify_one();
    } else {
        drawing.unlock();
        present_now();
    }
    // Drawing that holds the GPU thread up holds up the game too (it waits for the GPU to take its commands): which
    // part took the time, for the first few.
    static int slow_frames = 0;
    const double total = milliseconds(overlay_end - effects_start);
    if (total > 250 && slow_frames < 20) {
        ++slow_frames;
        log_line("renderer: drawing the window took %.0f ms: effects %.0f, overlay %.0f", total,
                 milliseconds(overlay_start - effects_start), milliseconds(overlay_end - overlay_start));
    }
}

}  // namespace xport::pc
