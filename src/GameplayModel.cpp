#include "GameplayModel.h"
#include "GameplayLimits.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <algorithm>
#include <array>

#include "cgltf/cgltf.h"

namespace {
constexpr size_t kParseLimit = 8 * 1024 * 1024;
constexpr size_t kVertexLimit = 65536;
constexpr size_t kIndexLimit = 196608;
struct Budget { size_t used = 0; };
struct alignas(std::max_align_t) Allocation { size_t size; };

void* allocate(void* context, cgltf_size size) {
    auto& budget = *static_cast<Budget*>(context);
    if (size > kParseLimit - budget.used || size > std::numeric_limits<size_t>::max() - sizeof(Allocation))
        return nullptr;
    auto* p = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
    if (!p) return nullptr;
    p->size = size;
    budget.used += size;
    return p + 1;
}

void release(void* context, void* pointer) {
    if (!pointer) return;
    auto* p = static_cast<Allocation*>(pointer) - 1;
    static_cast<Budget*>(context)->used -= p->size;
    std::free(p);
}

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool validAccessor(const cgltf_accessor* a, cgltf_type type, cgltf_component_type component,
                   size_t maxCount) {
    if (!a || a->type != type || a->component_type != component || a->normalized || a->is_sparse ||
        !a->buffer_view || a->count == 0 || a->count > maxCount || a->extensions_count)
        return false;
    const size_t element = cgltf_calc_size(type, component);
    const auto& view = *a->buffer_view;
    if (!element || a->stride < element || a->offset > view.size || element > view.size - a->offset)
        return false;
    return a->count - 1 <= (view.size - a->offset - element) / a->stride;
}

const unsigned char* element(const cgltf_accessor* a, size_t index) {
    return static_cast<const unsigned char*>(a->buffer_view->buffer->data) +
           a->buffer_view->offset + a->offset + a->stride * index;
}

float readFloat(const unsigned char* p) {
    const std::uint32_t bits = le32(p);
    float value;
    static_assert(sizeof(value) == sizeof(bits), "GLB requires 32-bit floats");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
}

namespace sf {
bool decodeGameplayModel(const std::string& input, GameplayModel& out, std::string& error) {
    out = {};
    error.clear();
    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
    if (input.size() < 28 || input.size() > kGameplayAssetLimit || le32(bytes) != 0x46546c67 ||
        le32(bytes + 4) != 2 || le32(bytes + 8) != input.size()) {
        error = "Invalid or oversized GLB header";
        return false;
    }
    const size_t jsonSize = le32(bytes + 12);
    if ((jsonSize & 3) || jsonSize > input.size() - 28 || le32(bytes + 16) != 0x4e4f534a) {
        error = "Invalid GLB JSON chunk";
        return false;
    }
    const size_t binHeader = 20 + jsonSize;
    const size_t binSize = le32(bytes + binHeader);
    if ((binSize & 3) || le32(bytes + binHeader + 4) != 0x004e4942 ||
        binSize != input.size() - binHeader - 8) {
        error = "Invalid GLB binary chunk";
        return false;
    }
    Budget budget;
    cgltf_options options{};
    options.type = cgltf_file_type_glb;
    options.memory = {allocate, release, &budget};
    cgltf_data* parsed = nullptr;
    if (cgltf_parse(&options, input.data(), input.size(), &parsed) != cgltf_result_success) {
        error = "Cannot parse GLB within memory budget";
        return false;
    }
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
    if (!data->asset.version || std::strcmp(data->asset.version, "2.0") != 0 ||
        (data->asset.min_version && std::strcmp(data->asset.min_version, "2.0") != 0) ||
        data->asset.extensions_count || data->buffers_count != 1 || data->buffers[0].uri ||
        data->buffers[0].extensions_count || data->buffers[0].size > binSize ||
        data->meshes_count == 0 || data->meshes_count > 32 ||
        data->nodes_count > 32 || data->scenes_count > 1 || data->images_count > 1 || data->textures_count > 1 ||
        data->materials_count > 1 || data->skins_count || data->animations_count || data->cameras_count > 1 ||
        data->lights_count || data->extensions_used_count || data->extensions_required_count ||
        data->data_extensions_count) {
        error = "Unsupported GLB features or external resources";
        return false;
    }
    for (size_t i = 0; i < data->meshes_count; ++i) {
        const auto& mesh = data->meshes[i];
        if (!mesh.primitives_count || mesh.primitives_count > 64 || mesh.extensions_count || mesh.weights_count) {
            error = "Unsupported GLB mesh";
            return false;
        }
    }
    struct PrimitiveInstance { const cgltf_primitive* primitive; const cgltf_node* node; };
    std::array<PrimitiveInstance, 64> instances{};
    size_t instanceCount = 0;
    const auto addMesh = [&](const cgltf_mesh& mesh, const cgltf_node* node) {
        if (mesh.primitives_count > instances.size() - instanceCount) return false;
        for (size_t i = 0; i < mesh.primitives_count; ++i)
            instances[instanceCount++] = {&mesh.primitives[i], node};
        return true;
    };
    for (size_t i = 0; i < data->nodes_count; ++i) {
        const auto& node = data->nodes[i];
        if (node.weights_count || node.has_mesh_gpu_instancing || node.extensions_count) {
            error = "Unsupported GLB node";
            return false;
        }
        const auto* ancestor = &node;
        for (size_t depth = 0; ancestor; ++depth, ancestor = ancestor->parent) {
            if (depth >= data->nodes_count) { error = "Cyclic GLB hierarchy"; return false; }
        }
        if (!node.mesh) continue;
        if (data->scenes_count) {
            const cgltf_node* root = &node;
            while (root->parent) root = root->parent;
            bool reachable = false;
            for (size_t j = 0; j < data->scenes[0].nodes_count; ++j)
                if (data->scenes[0].nodes[j] == root) reachable = true;
            if (!reachable) { error = "GLB mesh is not in the scene"; return false; }
        }
        if (!addMesh(*node.mesh, &node)) { error = "GLB primitive instance limit exceeded"; return false; }
    }
    if (!data->nodes_count && !data->scenes_count && data->meshes_count == 1 &&
        !addMesh(data->meshes[0], nullptr)) {
        error = "GLB primitive instance limit exceeded"; return false;
    }
    if (!instanceCount || (data->scenes_count && data->scenes[0].extensions_count)) {
        error = "GLB scene has no supported mesh nodes";
        return false;
    }
    data->buffers[0].data = const_cast<unsigned char*>(bytes + binHeader + 8);
    for (size_t i = 0; i < data->buffer_views_count; ++i) {
        const auto& view = data->buffer_views[i];
        if (view.buffer != &data->buffers[0] || view.has_meshopt_compression || view.extensions_count ||
            view.offset > data->buffers[0].size || view.size > data->buffers[0].size - view.offset) {
            error = "Invalid GLB buffer view";
            return false;
        }
    }
    for (size_t i = 0; i < data->images_count; ++i) {
        const auto& image = data->images[i];
        if (image.uri || !image.buffer_view || !image.mime_type ||
            std::strcmp(image.mime_type, "image/png") != 0 || image.extensions_count) {
            error = "GLB images must be embedded PNGs";
            return false;
        }
    }
    GameplayModel result;
    for (size_t primitiveIndex = 0; primitiveIndex < instanceCount; ++primitiveIndex) {
        const auto& primitive = *instances[primitiveIndex].primitive;
        const auto* meshNode = instances[primitiveIndex].node;
        if (primitive.material != instances[0].primitive->material) {
            error = "GLB primitives must share a material";
            return false;
        }
        if (primitive.type != cgltf_primitive_type_triangles || primitive.targets_count ||
            primitive.has_draco_mesh_compression || primitive.extensions_count || primitive.mappings_count) {
            error = "Unsupported GLB primitive";
            return false;
        }
        const cgltf_accessor *position = nullptr, *normal = nullptr, *uv = nullptr;
        for (size_t i = 0; i < primitive.attributes_count; ++i) {
            const auto& attr = primitive.attributes[i];
            if (attr.type == cgltf_attribute_type_position && attr.index == 0 && !position) position = attr.data;
            else if (attr.type == cgltf_attribute_type_normal && attr.index == 0 && !normal) normal = attr.data;
            else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0 && !uv) uv = attr.data;
            else { error = "Unsupported or duplicate GLB attribute"; return false; }
        }
        if (!validAccessor(position, cgltf_type_vec3, cgltf_component_type_r_32f, kVertexLimit) ||
            (normal && (!validAccessor(normal, cgltf_type_vec3, cgltf_component_type_r_32f, kVertexLimit) || normal->count != position->count)) ||
            (uv && (!validAccessor(uv, cgltf_type_vec2, cgltf_component_type_r_32f, kVertexLimit) || uv->count != position->count))) {
            error = "Invalid GLB vertex accessor";
            return false;
        }
        if (primitive.material && primitive.material->has_pbr_metallic_roughness &&
            primitive.material->pbr_metallic_roughness.base_color_texture.texture && !uv) {
            error = "Textured GLB primitives require UV coordinates";
            return false;
        }
        const auto* indices = primitive.indices;
        if (indices && (!validAccessor(indices, cgltf_type_scalar, indices->component_type, kIndexLimit) ||
            (indices->component_type != cgltf_component_type_r_8u && indices->component_type != cgltf_component_type_r_16u &&
             indices->component_type != cgltf_component_type_r_32u))) {
            error = "Invalid GLB index accessor";
            return false;
        }
        const size_t indexCount = indices ? indices->count : position->count;
        if (indexCount % 3 || indexCount > kIndexLimit) { error = "Invalid triangle count"; return false; }
        const size_t baseVertex = result.vertices.size() / 8;
        const size_t baseIndex = result.indices.size();
        if (position->count > kVertexLimit - baseVertex || indexCount > kIndexLimit - baseIndex) {
            error = "GLB aggregate geometry limit exceeded";
            return false;
        }
        if (primitiveIndex == 0 && primitive.material) {
            const auto& material = *primitive.material;
            if (!material.has_pbr_metallic_roughness || material.extensions_count) {
                error = "Unsupported GLB material";
                return false;
            }
            for (int i = 0; i < 4; ++i) {
                const float factor = material.pbr_metallic_roughness.base_color_factor[i];
                if (!std::isfinite(factor) || factor < 0 || factor > 1) {
                    error = "Invalid GLB base colour";
                    return false;
                }
                result.baseColour[i] = factor;
            }
            const auto& view = material.pbr_metallic_roughness.base_color_texture;
            if (view.texture) {
                if (view.texcoord != 0 || view.has_transform || !uv || !view.texture->image ||
                    view.texture->has_basisu || view.texture->has_webp) {
                    error = "Unsupported GLB texture";
                    return false;
                }
                const auto& image = *view.texture->image;
                if (image.uri || !image.buffer_view || !image.mime_type ||
                    std::strcmp(image.mime_type, "image/png") != 0 || image.extensions_count) {
                    error = "GLB texture must be an embedded PNG";
                    return false;
                }
                const auto& imageView = *image.buffer_view;
                if (imageView.size > kGameplayAssetLimit) { error = "GLB texture is too large"; return false; }
                try {
                    const auto* imageBytes = static_cast<const char*>(imageView.buffer->data) + imageView.offset;
                    if (!decodeGameplayTexture(std::string(imageBytes, imageView.size), result.embeddedTexture, error))
                        return false;
                } catch (const std::bad_alloc&) {
                    error = "Cannot allocate GLB texture";
                    return false;
                }
            }
        }
        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (meshNode) cgltf_node_transform_world(meshNode, matrix);
        for (float v : matrix) if (!std::isfinite(v) || std::abs(v) > 10000) {
            error = "Invalid GLB node transform";
            return false;
        }
        // Cofactor matrix is the inverse transpose up to a positive scalar.
        const double normalMatrix[] = {
            double(matrix[5])*matrix[10]-double(matrix[6])*matrix[9],
            double(matrix[6])*matrix[8]-double(matrix[4])*matrix[10],
            double(matrix[4])*matrix[9]-double(matrix[5])*matrix[8],
            double(matrix[9])*matrix[2]-double(matrix[10])*matrix[1],
            double(matrix[10])*matrix[0]-double(matrix[8])*matrix[2],
            double(matrix[8])*matrix[1]-double(matrix[9])*matrix[0],
            double(matrix[1])*matrix[6]-double(matrix[2])*matrix[5],
            double(matrix[2])*matrix[4]-double(matrix[0])*matrix[6],
            double(matrix[0])*matrix[5]-double(matrix[1])*matrix[4]
        };
        const double determinant = matrix[0]*normalMatrix[0] + matrix[1]*normalMatrix[1] + matrix[2]*normalMatrix[2];
        const double maxScale = std::max({std::hypot(double(matrix[0]), double(matrix[1]), double(matrix[2])),
                                          std::hypot(double(matrix[4]), double(matrix[5]), double(matrix[6])),
                                          std::hypot(double(matrix[8]), double(matrix[9]), double(matrix[10]))});
        if (matrix[3] != 0 || matrix[7] != 0 || matrix[11] != 0 || matrix[15] != 1 ||
            maxScale < 1e-6 || determinant <= maxScale * maxScale * maxScale * 1e-8) {
            error = "Unsupported GLB singular, reflected or ill-conditioned transform";
            return false;
        }
        try {
            result.vertices.reserve((baseVertex + position->count) * 8);
            result.indices.reserve(baseIndex + indexCount);
            for (size_t i = 0; i < position->count; ++i) {
                const cgltf_accessor* attributes[] = {position, normal, uv};
                for (int attribute = 0; attribute < 3; ++attribute) {
                    const auto* a = attributes[attribute];
                    const int components = attribute == 2 ? 2 : 3;
                    float values[3]{};
                    for (int j = 0; j < components; ++j) {
                        const float value = a ? readFloat(element(a, i) + j * 4) :
                            (attribute == 1 && j == 1 ? 1.0f : 0.0f);
                        if (!std::isfinite(value) || std::abs(value) > 10000) {
                            error = "Invalid GLB vertex value";
                            return false;
                        }
                        values[j] = value;
                    }
                    if (attribute == 0) {
                        float transformed[3];
                        for (int j = 0; j < 3; ++j) {
                            transformed[j] = matrix[j] * values[0] + matrix[4+j] * values[1] +
                                             matrix[8+j] * values[2] + matrix[12+j];
                            if (!std::isfinite(transformed[j]) || std::abs(transformed[j]) > 10000) {
                                error = "GLB transformed vertex out of range";
                                return false;
                            }
                        }
                        std::copy(transformed, transformed + 3, values);
                    } else if (attribute == 1) {
                        double transformed[3];
                        for (int j = 0; j < 3; ++j)
                            transformed[j] = normalMatrix[j]*values[0] + normalMatrix[3+j]*values[1] +
                                             normalMatrix[6+j]*values[2];
                        const double length = std::hypot(transformed[0], transformed[1], transformed[2]);
                        if (length > 1e-20)
                            for (int j = 0; j < 3; ++j) values[j] = static_cast<float>(transformed[j] / length);
                    }
                    result.vertices.insert(result.vertices.end(), values, values + components);
                }
            }
            for (size_t i = 0; i < indexCount; ++i) {
                std::uint32_t index = static_cast<std::uint32_t>(i);
                if (indices) {
                    const auto* p = element(indices, i);
                    if (indices->component_type == cgltf_component_type_r_8u) index = p[0];
                    else if (indices->component_type == cgltf_component_type_r_16u) index = p[0] | (p[1] << 8);
                    else index = le32(p);
                }
                if (index >= position->count) { error = "GLB index out of range"; return false; }
                result.indices.push_back(static_cast<std::uint32_t>(baseVertex) + index);
            }
            if (!normal) {
                for (size_t i = baseIndex; i < baseIndex + indexCount; i += 3) {
                    float* a = result.vertices.data() + result.indices[i] * 8;
                    float* b = result.vertices.data() + result.indices[i + 1] * 8;
                    float* c = result.vertices.data() + result.indices[i + 2] * 8;
                    const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
                    const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
                    const double nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
                    const double length = std::sqrt(nx*nx + ny*ny + nz*nz);
                    if (length > 1e-12) {
                        for (float* v : {a, b, c}) {
                            v[3] = static_cast<float>(nx / length);
                            v[4] = static_cast<float>(ny / length);
                            v[5] = static_cast<float>(nz / length);
                        }
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            error = "Cannot allocate GLB geometry";
            return false;
        }
    }
    out = std::move(result);
    return true;
}
}  // namespace sf
