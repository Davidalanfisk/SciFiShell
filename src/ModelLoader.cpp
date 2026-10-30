#include "ModelLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"
#include "stb_image.h"

namespace sf {

namespace {
constexpr size_t kMaxVertices = 700000;

constexpr int kTile = 256;

struct Ctx {
    const cgltf_data* data;
    int grid;
};

void walk(const Ctx& cx, const cgltf_node* node, std::vector<float>& v, std::vector<uint32_t>& idx) {
    if (node->mesh) {
        float m[16];
        cgltf_node_transform_world(node, m);
        for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node->mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position && !pos) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal && !nrm) nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && !uv && at.index == 0) uv = at.data;
            }
            if (!pos || v.size() / 8 + pos->count > kMaxVertices) continue;
            const uint32_t base = static_cast<uint32_t>(v.size() / 8);
            int tile = 0;
            if (prim.material) tile = static_cast<int>(prim.material - cx.data->materials) % (cx.grid * cx.grid);
            const float inset = 2.0f / kTile;
            for (cgltf_size i = 0; i < pos->count; ++i) {
                float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, t[2] = {0, 0};
                cgltf_accessor_read_float(pos, i, p, 3);
                if (nrm) cgltf_accessor_read_float(nrm, i, n, 3);
                if (uv) cgltf_accessor_read_float(uv, i, t, 2);
                const float wx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
                const float wy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
                const float wz = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
                float nx = m[0] * n[0] + m[4] * n[1] + m[8] * n[2];
                float ny = m[1] * n[0] + m[5] * n[1] + m[9] * n[2];
                float nz = m[2] * n[0] + m[6] * n[1] + m[10] * n[2];
                const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (l > 1e-8f) { nx /= l; ny /= l; nz /= l; }
                for (float& q : t)
                    if (q < 0.0f || q > 1.0f) q -= std::floor(q);
                t[0] = (tile % cx.grid + inset + t[0] * (1.0f - 2 * inset)) / cx.grid;
                t[1] = (tile / cx.grid + inset + t[1] * (1.0f - 2 * inset)) / cx.grid;
                v.insert(v.end(), {wx, wy, wz, nx, ny, nz, t[0], t[1]});
            }
            const size_t first = idx.size();
            if (prim.indices) {
                for (cgltf_size i = 0; i + 2 < prim.indices->count; i += 3)
                    for (int k = 0; k < 3; ++k) idx.push_back(base + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i + k)));
            } else {
                for (cgltf_size i = 0; i + 2 < pos->count; i += 3)
                    for (int k = 0; k < 3; ++k) idx.push_back(base + static_cast<uint32_t>(i + k));
            }
            if (!nrm) {  // no normals in the file: give each triangle's corners its face normal
                for (size_t i = first; i + 2 < idx.size(); i += 3) {
                    float* a = &v[idx[i] * 8L];
                    float* b = &v[idx[i + 1] * 8L];
                    float* c = &v[idx[i + 2] * 8L];
                    Vec3 n = normalize(cross(Vec3{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, Vec3{c[0] - a[0], c[1] - a[1], c[2] - a[2]}));
                    for (float* q : {a, b, c}) { q[3] = n.x; q[4] = n.y; q[5] = n.z; }
                }
            }
        }
    }
    for (cgltf_size i = 0; i < node->children_count; ++i) walk(cx, node->children[i], v, idx);
}
}  // namespace

bool loadGltfMesh(const std::string& path, std::vector<float>& vertices, std::vector<uint32_t>& indices, Vec3& size, Bitmap* atlas) {
    cgltf_options opt;
    std::memset(&opt, 0, sizeof opt);
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }
    vertices.clear();
    indices.clear();
    // Every material becomes one tile of a square atlas: its base-colour texture (if embedded) times its colour factor.
    const int grid = std::clamp(static_cast<int>(std::ceil(std::sqrt(static_cast<double>(std::max<cgltf_size>(1, data->materials_count))))), 1, 4);
    const Ctx ctx{data, grid};
    if (data->scene) {
        for (cgltf_size i = 0; i < data->scene->nodes_count; ++i) walk(ctx, data->scene->nodes[i], vertices, indices);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent) walk(ctx, &data->nodes[i], vertices, indices);
    }
    if (atlas) {
        atlas->w = atlas->h = grid * kTile;
        atlas->rgba.assign(static_cast<size_t>(atlas->w) * atlas->h * 4, 255);
        for (int m = 0; m < grid * grid; ++m) {
            const cgltf_material* mat = m < static_cast<int>(data->materials_count) ? &data->materials[m] : nullptr;
            float f[4] = {0.8f, 0.8f, 0.8f, 1.0f};
            unsigned char* px = nullptr;
            int tw = 0, th = 0, tc = 0;
            if (mat && (mat->has_pbr_metallic_roughness || mat->has_pbr_specular_glossiness)) {
                const cgltf_texture* tex = nullptr;
                if (mat->has_pbr_metallic_roughness) {
                    std::memcpy(f, mat->pbr_metallic_roughness.base_color_factor, sizeof f);
                    tex = mat->pbr_metallic_roughness.base_color_texture.texture;
                } else {
                    std::memcpy(f, mat->pbr_specular_glossiness.diffuse_factor, sizeof f);
                    tex = mat->pbr_specular_glossiness.diffuse_texture.texture;
                }
                if (tex && tex->image && tex->image->buffer_view) {
                    const cgltf_buffer_view* bv = tex->image->buffer_view;
                    px = stbi_load_from_memory(static_cast<const unsigned char*>(bv->buffer->data) + bv->offset, static_cast<int>(bv->size), &tw, &th, &tc, 4);
                }
            }            for (int y = 0; y < kTile; ++y)
                for (int x = 0; x < kTile; ++x) {
                    unsigned char* o = &atlas->rgba[(static_cast<size_t>((m / grid) * kTile + y) * atlas->w + (m % grid) * kTile + x) * 4];
                    float c[3] = {1, 1, 1};
                    if (px) {
                        const unsigned char* s = px + (static_cast<size_t>(y * th / kTile) * tw + x * tw / kTile) * 4;
                        for (int k = 0; k < 3; ++k) c[k] = s[k] / 255.0f;
                    }
                    for (int k = 0; k < 3; ++k) o[k] = static_cast<unsigned char>(std::clamp(c[k] * f[k], 0.0f, 1.0f) * 255.0f + 0.5f);
                }            if (px) stbi_image_free(px);
        }
    }
    cgltf_free(data);
    if (vertices.empty() || indices.empty()) return false;

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < vertices.size(); i += 8)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], vertices[i + k]);
            hi[k] = std::max(hi[k], vertices[i + k]);
        }
    const float h = std::max(1e-6f, hi[1] - lo[1]), s = 1.0f / h;
    const float cx = (lo[0] + hi[0]) * 0.5f, cz = (lo[2] + hi[2]) * 0.5f;
    for (size_t i = 0; i < vertices.size(); i += 8) {
        vertices[i] = (vertices[i] - cx) * s;
        vertices[i + 1] = (vertices[i + 1] - lo[1]) * s;
        vertices[i + 2] = (vertices[i + 2] - cz) * s;
    }
    size = {(hi[0] - lo[0]) * s, 1.0f, (hi[2] - lo[2]) * s};
    return true;
}

}  // namespace sf
