#pragma once
// Loads a .glb / .gltf file into one triangle list (pos, normal, uv). The model is centred on x/z, sits on y = 0 and is
// scaled so that its height is 1 (`size` returns the resulting x/y/z extents, so width/depth are relative to the height).
#include <cstdint>
#include <string>
#include <vector>

#include "Utils.h"

namespace sf {

// The two front wheel subtrees (nodes named WheelFrontL* / WheelFrontR*) can be split out so they can be steered; the pivot is the wheel's vertical axis.
struct WheelPart {
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    Vec3 pivot{0, 0, 0};
};

bool loadGltfMesh(const std::string& path, std::vector<float>& vertices, std::vector<uint32_t>& indices, Vec3& size, Bitmap* atlas = nullptr, std::vector<WheelPart>* wheels = nullptr);

}  // namespace sf
