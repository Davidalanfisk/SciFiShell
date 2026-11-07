#include "AppShell.h"
#include <SDL.h>

namespace sf {

Plugin AppShell::makeParksPlugin() {
    Plugin plugin{"org.scifishell.parks", "PARKS AND TOYS", "Parks and toys; trusted native code, not sandboxed", "1.0.0",
            [this] { city_->setParksEnabled(true); },
            [this] { city_->setParksEnabled(false); scoreT_ = 0; scoreLabel_.reset(); }};
    plugin.prepare = [this](std::string& error) {
        char* base = SDL_GetBasePath();
        if (!base) { error = std::string("Cannot locate parks module: ") + SDL_GetError(); return false; }
        const auto path = std::filesystem::u8path(base) / SCIFI_PARKS_MODULE_FILENAME;
        SDL_free(base);
        return city_->prepareParksModule(path, error);
    };
    plugin.cancelPrepare = [this] { city_->releaseParksModule(); };
    return plugin;
}

}  // namespace sf
