#include "GameplaySceneRenderer.h"

#include <cmath>
#include <iterator>
#include <new>
#include <algorithm>

namespace sf {
namespace {
Mat4 transform(const PluginScriptRuntime::Box& box) {
    const float z = static_cast<float>(box.rotateZ);
    Mat4 rz = Mat4::identity();
    rz.m[0] = std::cos(z); rz.m[1] = std::sin(z);
    rz.m[4] = -std::sin(z); rz.m[5] = std::cos(z);
    return Mat4::translate({static_cast<float>(box.x), static_cast<float>(box.y), static_cast<float>(box.z)}) *
           Mat4::rotateY(static_cast<float>(box.rotateY)) *
           Mat4::rotateX(static_cast<float>(box.rotateX)) * rz *
           Mat4::scale({static_cast<float>(box.width), static_cast<float>(box.height), static_cast<float>(box.depth)});
}
Vec3 colour(const PluginScriptRuntime::Box& box) {
    return {static_cast<float>(box.red), static_cast<float>(box.green), static_cast<float>(box.blue)};
}
}

GameplaySceneRenderer::~GameplaySceneRenderer() { clear(); }

void GameplaySceneRenderer::clear() {
    renderer_.releaseGameplayModels(owner_);
    models_.clear();
    preparedSession_ = nullptr;
    preparedGeneration_ = 0;
}

bool GameplaySceneRenderer::prepare(const GameplaySession& session, std::string& error) {
    if (!owner_ || !session.running() || !session.package()) {
        clear();
        error = "Gameplay scene is not ready";
        return false;
    }
    try {
        const auto instances = session.models();
        std::set<Key> needed;
        for (const auto& instance : instances) needed.emplace(instance.model, instance.texture);
        bool changed = preparedSession_ != &session || preparedGeneration_ != session.generation() ||
                       needed.size() != models_.size();
        if (!changed) for (const auto& key : needed) if (!models_.count(key)) { changed = true; break; }
        if (changed) {
            clear();
            const auto& package = *session.package();
            for (const auto& key : needed) {
                if (key.first == 0 || (key.first > 0 && static_cast<size_t>(key.first) > package.models.size()) ||
                    key.second < 0 || static_cast<size_t>(key.second) > package.textures.size()) {
                    error = "Gameplay scene references an unavailable asset";
                    clear();
                    return false;
                }
                const GameplayModel* model = key.first < 0 ? session.proceduralModel(-key.first) :
                    &std::next(package.models.begin(), key.first - 1)->second;
                if (!model) { error = "Gameplay scene references an unavailable mesh"; clear(); return false; }
                const GameplayTexture* texture = key.second ?
                    &std::next(package.textures.begin(), key.second - 1)->second : nullptr;
                uint64_t handle = 0;
                if (!renderer_.createGameplayModel(owner_, *model, texture, handle, error)) {
                    clear();
                    return false;
                }
                models_.emplace(key, handle);
            }
        }
        preparedSession_ = &session;
        preparedGeneration_ = session.generation();
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        clear();
        error = "Cannot allocate gameplay scene";
        return false;
    }
}

bool GameplaySceneRenderer::draw(const GameplaySession& session, const GpuLabel& cubeLabel, std::string& error) {
    if (!owner_ || !session.running() || !session.package() || !cubeLabel.solidSRB ||
        preparedSession_ != &session || preparedGeneration_ != session.generation()) {
        error = "Gameplay scene is not ready";
        return false;
    }
    try {
        const auto instances = session.models();
        const auto boxes = session.boxes();
        for (const auto& box : boxes)
            if (!box.material) renderer_.drawCube(cubeLabel, transform(box), colour(box), false, false, -1.0f);
        for (const auto& instance : instances) {
            if (instance.transform.material) continue;
            const auto it = models_.find({instance.model, instance.texture});
            if (it == models_.end() ||
                !renderer_.drawGameplayModel(owner_, it->second, transform(instance.transform), colour(instance.transform))) {
                error = "Gameplay model draw failed";
                return false;
            }
        }
        struct Transparent { PluginScriptRuntime::Box box; uint64_t model; };
        std::vector<Transparent> translucent;
        translucent.reserve(boxes.size() + instances.size());
        for (const auto& box : boxes) if (box.material && box.opacity > 0) translucent.push_back({box, 0});
        for (const auto& instance : instances) if (instance.transform.material && instance.transform.opacity > 0) {
            const auto it = models_.find({instance.model, instance.texture});
            if (it == models_.end()) { error = "Gameplay model draw failed"; return false; }
            translucent.push_back({instance.transform, it->second});
        }
        // Sort by the current render eye (including reflection/VR passes), not the player's previous-frame pose.
        const Vec3 eye = renderer_.viewEye();
        const auto distance = [&](const Transparent& value) {
            const auto& b = value.box;
            return (b.x-eye.x)*(b.x-eye.x) + (b.y-eye.y)*(b.y-eye.y) + (b.z-eye.z)*(b.z-eye.z);
        };
        std::stable_sort(translucent.begin(), translucent.end(),
            [&](const Transparent& a, const Transparent& b) { return distance(a) > distance(b); });
        for (const auto& item : translucent) {
            const auto& b = item.box;
            if (!item.model)
                renderer_.drawGameplayBox(cubeLabel, transform(b), colour(b), static_cast<float>(b.opacity), static_cast<float>(b.emission));
            else if (!renderer_.drawGameplayModel(owner_, item.model, transform(b), colour(b), true,
                                                  static_cast<float>(b.opacity), static_cast<float>(b.emission))) {
                error = "Translucent gameplay model draw failed";
                return false;
            }
        }
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay scene";
        return false;
    }
}
}  // namespace sf
