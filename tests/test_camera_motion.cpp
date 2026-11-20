// Camera motion vectors (camera_motion.h): the reprojection of a moving camera, and its shader compiles.

#include <windows.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

#include "pc/camera_motion.h"
#include "test.h"

using namespace xport::pc;

namespace {

constexpr float kClipMax = 16777215.0f;  // a 24-bit depth buffer

// A camera as a game builds it: the view (yaw about y, then the eye's position), a perspective (D3D-style, depth 0
// to 1), then the viewport to 640x480 pixels and the depth buffer's units.
Mat4 camera(float eye_x, float eye_y, float eye_z, float yaw) {
    Mat4 view;
    const float c = std::cos(yaw), s = std::sin(yaw);
    view(0, 0) = c, view(0, 2) = -s, view(2, 0) = s, view(2, 2) = c;
    view(0, 3) = -(c * eye_x - s * eye_z), view(1, 3) = -eye_y, view(2, 3) = -(s * eye_x + c * eye_z);
    const float near_plane = 0.5f, far_plane = 2000.0f, f = 1 / std::tan(0.5f * 1.2f), aspect = 640.0f / 480;
    Mat4 projection;
    projection(0, 0) = f / aspect, projection(1, 1) = f;
    projection(2, 2) = far_plane / (far_plane - near_plane), projection(2, 3) = -near_plane * far_plane / (far_plane - near_plane);
    projection(3, 2) = 1, projection(3, 3) = 0;
    Mat4 viewport;
    viewport(0, 0) = 320, viewport(0, 3) = 320, viewport(1, 1) = -240, viewport(1, 3) = 240, viewport(2, 2) = kClipMax;
    return viewport * projection * view;
}

struct Screen {
    float x, y, z;
};

Screen project(const Mat4& m, float x, float y, float z) {
    const float v[4] = {x, y, z, 1};
    float p[4] = {};
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 4; ++k) p[i] += m(i, k) * v[k];
    }
    return {p[0] / p[3], p[1] / p[3], p[2] / p[3]};
}

bool close(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

}  // namespace

int main() {
    // The inverse.
    const Mat4 a = camera(3, 1.5f, -7, 0.4f);
    Mat4 inverse;
    CHECK(invert(a, inverse));
    const Mat4 identity = a * inverse;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) CHECK(close(identity(i, j), i == j ? 1.0f : 0.0f, 1e-3f));
    }
    Mat4 singular{};
    std::memset(singular.m, 0, sizeof(singular.m));
    CHECK(!invert(singular, inverse));

    // A still camera: no motion anywhere.
    const Mat4 still = reprojection(a, a);
    for (const Screen s : {Screen{10, 20, 0.9f * kClipMax}, Screen{320, 240, 0.99f * kClipMax}, Screen{630, 470, 0.5f * kClipMax}}) {
        const ScreenMotion m = screen_motion(still, s.x, s.y, s.z);
        CHECK(m.valid && close(m.x, 0, 0.01f) && close(m.y, 0, 0.01f));
    }

    // A camera that moves and turns: each point's vector leads back to where the previous camera saw it.
    const Mat4 before = camera(0, 1.5f, -10, 0.0f), now = camera(0.8f, 1.6f, -9.2f, 0.05f);
    const Mat4 back = reprojection(before, now);
    const float points[][3] = {{0, 1, 5}, {-4, 0, 12}, {6, 3, 40}, {1, 0.2f, 300}, {-30, 10, 900}};
    for (const auto& p : points) {
        const Screen s = project(now, p[0], p[1], p[2]), b = project(before, p[0], p[1], p[2]);
        const ScreenMotion m = screen_motion(back, s.x, s.y, s.z);
        CHECK(m.valid);
        CHECK(close(m.x, b.x - s.x, 0.05f));
        CHECK(close(m.y, b.y - s.y, 0.05f));
    }
    // A turn alone moves far points as much as near ones; a step sideways moves near points more.
    const Mat4 turned = reprojection(camera(0, 1.5f, -10, 0.0f), camera(0, 1.5f, -10, 0.02f));
    const Screen near_point = project(camera(0, 1.5f, -10, 0.02f), 0, 1.5f, 0);
    const Screen far_point = project(camera(0, 1.5f, -10, 0.02f), 0, 1.5f, 1500);
    const float near_turn = screen_motion(turned, near_point.x, near_point.y, near_point.z).x;
    const float far_turn = screen_motion(turned, far_point.x, far_point.y, far_point.z).x;
    CHECK(close(near_turn, far_turn, 0.1f) && std::fabs(near_turn) > 1);
    const Mat4 stepped = reprojection(camera(0, 1.5f, -10, 0), camera(0.5f, 1.5f, -10, 0));
    const Screen near_step = project(camera(0.5f, 1.5f, -10, 0), 0, 1.5f, 0);
    const Screen far_step = project(camera(0.5f, 1.5f, -10, 0), 0, 1.5f, 1500);
    CHECK(std::fabs(screen_motion(stepped, near_step.x, near_step.y, near_step.z).x) >
          10 * std::fabs(screen_motion(stepped, far_step.x, far_step.y, far_step.z).x));

    // A point the previous camera had behind it.
    const Mat4 about_turn = reprojection(camera(0, 1.5f, -10, 3.14159f), camera(0, 1.5f, -10, 0));
    const Screen ahead = project(camera(0, 1.5f, -10, 0), 0, 1.5f, 20);
    CHECK(!screen_motion(about_turn, ahead.x, ahead.y, ahead.z).valid);

    // The shader compiles for the renderer's pixel shader model.
    const char* source = camera_motion_shader();
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "camera motion", nullptr, nullptr, "main", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    CHECK(SUCCEEDED(hr) && code);
    if (code) code->Release();
    if (errors) errors->Release();
    return test_result();
}
