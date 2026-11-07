#include "AppShell.h"

namespace sf {

Plugin AppShell::makeParksPlugin() {
    return {"org.scifishell.parks", "PARKS AND TOYS", "Parks, ducks, hot dogs and ice cream", "1.0.0",
            [this] { city_->setParksEnabled(true); },
            [this] { city_->setParksEnabled(false); scoreT_ = 0; scoreLabel_.reset(); }};
}

}  // namespace sf
