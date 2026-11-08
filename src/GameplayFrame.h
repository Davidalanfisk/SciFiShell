#pragma once

#include "GameplayInput.h"
#include "GameplaySession.h"

namespace sf {

template <typename Fail>
bool runGameplayFrame(GameplaySession& session, GameplayInput& inputState, bool inputPermitted,
                      bool inputAvailable, float x, float y, float z, bool click, bool clickUp,
                      double dt, std::string& error, Fail fail) {
    bool ok = inputState.update(inputPermitted, inputAvailable, x, y, z, click, clickUp,
                                [&](const char* name, double value) {
                                    return session.input(name, value, error);
                                });
    if (ok) ok = session.tick(dt, error);
    if (!ok) fail(error);
    return ok;
}

}  // namespace sf
