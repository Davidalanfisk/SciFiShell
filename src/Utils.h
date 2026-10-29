#pragma once
// Math, text rasterisation and filesystem helpers shared by every subsystem.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sf {

constexpr float kPi = 3.14159265358979f;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    float l = length(a);
    return l > 1e-8f ? a * (1.0f / l) : Vec3{0, 0, -1};
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Distance at which a w x h panel fills `frac` of the screen (limited by whichever axis is tighter).
inline float fitDistance(float w, float h, float aspect, float tanHalf, float frac = 0.85f) {
    const float perUnit = 2.0f * tanHalf * frac;
    const float d = std::fmax(h / perUnit, w / (perUnit * aspect));
    return d < 0.9f ? 0.9f : d;
}
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Column-major 4x4 matrix (m[col * 4 + row]) using column vectors. The memory layout
// matches HLSL's default column_major cbuffer packing, so it can be uploaded directly.
struct Mat4 {
    float m[16];

    static Mat4 identity();
    static Mat4 translate(Vec3 t);
    static Mat4 scale(Vec3 s);
    static Mat4 rotateX(float a);
    static Mat4 rotateY(float a);
    // Right-handed, looks down -Z, clip depth 0..1.
    static Mat4 perspective(float fovY, float aspect, float zn, float zf);
    // Asymmetric projection from tangents of the half angles (OpenXR fov).
    static Mat4 perspectiveTan(float tanL, float tanR, float tanU, float tanD, float zn, float zf);
    static Mat4 fromPose(float qx, float qy, float qz, float qw, Vec3 p);

    Mat4 operator*(const Mat4& b) const;
    Vec3 transformPoint(Vec3 p) const;
    Vec3 transformDir(Vec3 d) const;
    Mat4 rigidInverse() const;  // valid for rotation + translation only
};

struct Bitmap {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// Rasterises ASCII text (upper-cased, built-in 5x7 font) into a white RGBA bitmap whose alpha is
// coverage. When minW/minH are non-zero the text is centred inside a bitmap of at least that size.
Bitmap rasterizeLines(const std::vector<std::string>& lines, int scale, int minW, int minH);

struct DirEntry {
    std::string name;
    std::string path;
    bool isDir = false;
};
std::string homeDirectory();
std::string parentDirectory(const std::string& path);
// Parent folder, or "" (the drives view) when `path` is a filesystem root.
std::string parentOrDrives(const std::string& path);
std::vector<DirEntry> listDrives();
std::vector<DirEntry> listDirectory(const std::string& path, size_t maxEntries);

}  // namespace sf
