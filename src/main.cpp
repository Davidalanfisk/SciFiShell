#include <cstdio>
#include <cstdlib>
#include <exception>

#include "AppShell.h"
#ifdef SCIFI_HAS_CEF
#include "WebBrowser.h"
#endif

#include <SDL.h>  // defines SDL_main on some platforms; keeps main() portable

// Crashes are appended to scifishell.log in the working directory so they are not silent.
static void logFatal(const char* what) {
    if (FILE* f = std::fopen("scifishell.log", "a")) {
        std::fprintf(f, "FATAL: %s\n", what);
        std::fclose(f);
    }
    std::fprintf(stderr, "FATAL: %s\n", what);
}

int main(int, char**) {
#ifdef SCIFI_HAS_CEF
    // The exe doubles as CEF's renderer/GPU/utility subprocess; those must never reach SDL or the app.
    int childExit = sf::WebBrowser::executeSubprocess();
    if (childExit >= 0) return childExit;
#endif
    std::set_terminate([] {
        logFatal("unhandled exception / terminate");
        std::abort();
    });
    try {
        sf::AppShell app;
        if (!app.init()) {
            logFatal("failed to initialise");
            return 1;
        }
        return app.run();
    } catch (const std::exception& e) {
        logFatal(e.what());
    } catch (...) {
        logFatal("unknown exception");
    }
    return 1;
}
