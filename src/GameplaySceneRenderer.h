#pragma once

#include "GameplaySession.h"
#include "Renderer.h"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace sf {
// The renderer must outlive this object; release resources between render targets.
class GameplaySceneRenderer {
public:
    GameplaySceneRenderer(Renderer& renderer, uint64_t owner) : renderer_(renderer), owner_(owner) {}
    ~GameplaySceneRenderer();
    GameplaySceneRenderer(const GameplaySceneRenderer&) = delete;
    GameplaySceneRenderer& operator=(const GameplaySceneRenderer&) = delete;

    // Prepare and clear must be called between render targets. Draw may be called for each view.
    bool prepare(const GameplaySession& session, std::string& error);
    bool draw(const GameplaySession& session, const GpuLabel& cubeLabel, std::string& error);
    void clear();

private:
    using Key = std::pair<int, int>;
    Renderer& renderer_;
    uint64_t owner_;
    const GameplaySession* preparedSession_ = nullptr;
    uint64_t preparedGeneration_ = 0;
    std::map<Key, uint64_t> models_;
};
}  // namespace sf
