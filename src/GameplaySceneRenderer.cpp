#include "GameplaySceneRenderer.h"

#include <cmath>
#include <iterator>
#include <new>

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
            renderer_.drawCube(cubeLabel, transform(box), colour(box), false, false, -1.0f);
        for (const auto& instance : instances) {
            const auto it = models_.find({instance.model, instance.texture});
            if (it == models_.end() ||
                !renderer_.drawGameplayModel(owner_, it->second, transform(instance.transform), colour(instance.transform))) {
                error = "Gameplay model draw failed";
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
