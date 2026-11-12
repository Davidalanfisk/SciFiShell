#include "ModelThumbnail.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

#include "GameplayModel.h"

namespace sf {
bool renderModelThumbnail(const std::string& data, Bitmap& out, std::string& error,
                          const std::function<bool()>& cancelled) {
    out = {};
    error.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    auto stopped = [&] {
        if (cancelled && cancelled()) { error = "Model preview cancelled"; return true; }
        if (std::chrono::steady_clock::now() > deadline) { error = "Model preview time budget exceeded"; return true; }
        return false;
    };
    if (stopped()) return false;
    if (data.size() > kModelPreviewBytes) { error = "Model preview exceeds 8 MiB"; return false; }
    GameplayModel model;
    if (!decodeGameplayModel(data, model, error) || stopped()) return false;

    struct Vertex { double x, y, z, u, v; };
    std::vector<Vertex> vertices;
    vertices.reserve(model.vertices.size() / 8);
    std::array<double, 3> lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (size_t i = 0; i < model.vertices.size(); i += 8) {
        // Fixed three-quarter view: all geometry shares the same orthographic scale and depth buffer.
        const double x = model.vertices[i], y = model.vertices[i + 1], z = model.vertices[i + 2];
        const double horizontal = (x - z) * 0.70710678, forward = (x + z) * 0.70710678;
        Vertex v{horizontal, y * 0.8660254 - forward * 0.5, y * 0.5 + forward * 0.8660254,
                 model.vertices[i + 6], model.vertices[i + 7]};
        vertices.push_back(v);
        const double coords[] = {v.x, v.y, v.z};
        for (int j = 0; j < 3; ++j) { lo[j] = std::min(lo[j], coords[j]); hi[j] = std::max(hi[j], coords[j]); }
    }
    constexpr int width = 384, height = 248;
    const double extent = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    if (vertices.empty() || !std::isfinite(extent) || extent < 1e-12) {
        error = "Model has no visible extent"; return false;
    }
    const double scale = std::min((width - 40) / std::max(hi[0] - lo[0], extent * 1e-6),
                                  (height - 40) / std::max(hi[1] - lo[1], extent * 1e-6));
    for (auto& v : vertices) {
        v.x = width * 0.5 + (v.x - (lo[0] + hi[0]) * 0.5) * scale;
        v.y = height * 0.5 - (v.y - (lo[1] + hi[1]) * 0.5) * scale;
    }
    Bitmap bitmap;
    bitmap.w = width; bitmap.h = height;
    bitmap.rgba.resize(width * height * 4);
    for (size_t p = 0; p < bitmap.rgba.size(); p += 4) {
        bitmap.rgba[p] = 18; bitmap.rgba[p + 1] = 25; bitmap.rgba[p + 2] = 38; bitmap.rgba[p + 3] = 255;
    }
    std::vector<double> depths(width * height, -std::numeric_limits<double>::infinity());
    size_t pixelWork = 0, visible = 0;
    auto edge = [](const Vertex& a, const Vertex& b, double x, double y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    };
    for (size_t i = 0; i < model.indices.size(); i += 3) {
        if (stopped()) return false;
        const auto& a = vertices[model.indices[i]];
        const auto& b = vertices[model.indices[i + 1]];
        const auto& c = vertices[model.indices[i + 2]];
        const double area = edge(a, b, c.x, c.y);
        if (std::fabs(area) < 1e-8) continue;
        const int x0 = std::clamp(int(std::floor(std::min({a.x, b.x, c.x}))), 0, width - 1);
        const int x1 = std::clamp(int(std::ceil(std::max({a.x, b.x, c.x}))), 0, width - 1);
        const int y0 = std::clamp(int(std::floor(std::min({a.y, b.y, c.y}))), 0, height - 1);
        const int y1 = std::clamp(int(std::ceil(std::max({a.y, b.y, c.y}))), 0, height - 1);
        pixelWork += size_t(x1 - x0 + 1) * (y1 - y0 + 1);
        if (pixelWork > 8000000) { error = "Model preview raster budget exceeded"; return false; }
        const double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
        const double ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        const double nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        const double light = length > 0 ? 0.35 + 0.65 * std::fabs((nx * 0.3 - ny * 0.5 + nz * 0.8124) / length) : 1;
        for (int y = y0; y <= y1; ++y) {
            if ((y & 15) == 0 && stopped()) return false;
            for (int x = x0; x <= x1; ++x) {
                const double wa = edge(b, c, x + 0.5, y + 0.5) / area;
                const double wb = edge(c, a, x + 0.5, y + 0.5) / area, wc = 1 - wa - wb;
                if (wa < 0 || wb < 0 || wc < 0) continue;
                const double depth = wa * a.z + wb * b.z + wc * c.z;
                const size_t p = size_t(y) * width + x;
                if (depth <= depths[p]) continue;
                const auto& texture = model.embeddedTexture;
                const uint8_t* texel = nullptr;
                if (texture.width && texture.height) {
                    const double u = wa * a.u + wb * b.u + wc * c.u, v = wa * a.v + wb * b.v + wc * c.v;
                    const int tx = std::min(int((u - std::floor(u)) * texture.width), int(texture.width) - 1);
                    const int ty = std::min(int((v - std::floor(v)) * texture.height), int(texture.height) - 1);
                    texel = &texture.rgba[(size_t(ty) * texture.width + tx) * 4];
                }
                const double alpha = model.baseColour[3] * (texel ? texel[3] / 255.0 : 1);
                if (alpha < 0.5) continue;
                depths[p] = depth;
                for (int channel = 0; channel < 3; ++channel)
                    bitmap.rgba[p * 4 + channel] = uint8_t(std::clamp(
                        (texel ? texel[channel] : 235.0) * model.baseColour[channel] * light, 0.0, 255.0));
                ++visible;
            }
        }
    }
    if (!visible) { error = "Model has no visible triangles"; return false; }
    out = std::move(bitmap);
    return true;
}
}
