#pragma once

// Motion vectors of the camera, for temporal upscalers and antialiasing (DLSS, FSR 2, a TAA of our own): where each
// pixel of this frame was on the screen in the previous one, if what it shows stood still in the world. Worked out
// from the depth buffer and the game's camera of the two frames, so it costs no work on the thread that reads the
// game's commands. What moves with the camera (Spider-Man, the player's car) needs per-draw vectors on top
// (sick-xport docs/UPSCALING-DLSS-FSR.md, P3 and P4).
//
// Spaces: the game's screen, as the NV2A has it after the perspective divide (x, y in the game's pixels; z from 0 to
// the depth buffer's maximum, clip_max; see clip_position in renderer_d3d11.cpp). A camera is the matrix that takes a
// point of the world (x, y, z, 1) to that screen before the divide (column vectors, m[row * 4 + column]), as the
// fixed-function composite matrix does.

#include <cmath>
#include <utility>

namespace xport::pc {

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float operator()(int row, int column) const { return m[row * 4 + column]; }
    float& operator()(int row, int column) { return m[row * 4 + column]; }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += a(i, k) * b(k, j);
            r(i, j) = sum;
        }
    }
    return r;
}

// The inverse (Gauss-Jordan, in double: a camera's matrix mixes pixels with depth units up to 2^24), or false.
inline bool invert(const Mat4& in, Mat4& out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) a[i][j] = in(i, j), a[i][j + 4] = i == j ? 1 : 0;
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row) {
            if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
        }
        if (std::fabs(a[pivot][column]) < 1e-30) return false;
        if (pivot != column) {
            for (int j = 0; j < 8; ++j) std::swap(a[pivot][j], a[column][j]);
        }
        const double scale = 1 / a[column][column];
        for (int j = 0; j < 8; ++j) a[column][j] *= scale;
        for (int row = 0; row < 4; ++row) {
            if (row == column || a[row][column] == 0) continue;
            const double factor = a[row][column];
            for (int j = 0; j < 8; ++j) a[row][j] -= factor * a[column][j];
        }
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) out(i, j) = static_cast<float>(a[i][j + 4]);
    }
    return true;
}

// From this frame's screen to the previous frame's: previous camera times the inverse of this one. Identity when the
// camera cannot be inverted (the vectors are then zero, as for a still camera).
inline Mat4 reprojection(const Mat4& previous_camera, const Mat4& camera) {
    Mat4 inverse;
    if (!invert(camera, inverse)) return Mat4{};
    return previous_camera * inverse;
}

struct ScreenMotion {
    float x = 0, y = 0;  // previous position minus this one, in the game's pixels
    bool valid = false;  // false when the point was behind the previous camera
};

// The motion of the screen point (x, y, z): what camera_motion_shader() computes per pixel.
inline ScreenMotion screen_motion(const Mat4& reprojection, float x, float y, float z) {
    const float v[4] = {x, y, z, 1};
    float p[4] = {};
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 4; ++k) p[i] += reprojection(i, k) * v[k];
    }
    if (!(p[3] > 1e-20f)) return {};
    return {p[0] / p[3] - x, p[1] / p[3] - y, true};
}

// The pixel shader that writes the motion vectors (R16G16_FLOAT, in render pixels, previous minus current, as DLSS
// and FSR 2 take them with a scale of 1) from the depth buffer, drawn with the full-screen triangle of the other
// passes. Constants: the reprojection's rows, then (clip_max, 1 / render scale, render scale, 0).
inline const char* camera_motion_shader() {
    return R"(
cbuffer CameraMotion : register(b0) {
    float4 reprojection[4];
    float4 depth_scale;  // x: clip_max, y: 1 / render scale, z: render scale
};
Texture2D<float> depth : register(t0);
float2 main(float4 position : SV_Position) : SV_Target {
    const float d = depth.Load(int3(position.xy, 0));
    // The game's screen: its pixels (the pixel centre, as the rasteriser samples it) and its depth units.
    const float4 here = float4(position.xy * depth_scale.y, d * depth_scale.x, 1);
    const float4 before = float4(dot(reprojection[0], here), dot(reprojection[1], here), dot(reprojection[2], here),
                                 dot(reprojection[3], here));
    if (!(before.w > 1e-20)) return float2(0, 0);
    return (before.xy / before.w - here.xy) * depth_scale.z;
}
)";
}

}  // namespace xport::pc
