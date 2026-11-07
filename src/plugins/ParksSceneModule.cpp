#include "NativePluginApi.h"
#include "ParksSceneApi.h"
#include <cstring>

#ifdef _WIN32
#define SF_EXPORT __declspec(dllexport)
#else
#define SF_EXPORT __attribute__((visibility("default")))
#endif

int renderParksScene(const SfParksSceneHostV1*, const SfParksBlockV1*, int*);
void* createParksToys();
void destroyParksToys(void*);
namespace {
void* create() { return createParksToys(); }
int start(void*) { return 1; }
void stop(void*) {}
void destroy(void* context) { destroyParksToys(context); }
const void* getInterface(void* context, const char* id) {
    static const SfParksSceneV1 api{sizeof(SfParksSceneV1), SF_PARKS_SCENE_ABI, renderParksScene};
    if (!id) return nullptr;
    if (std::strcmp(id, SF_PARKS_SCENE_INTERFACE) == 0) return &api;
    if (std::strcmp(id, SF_PARKS_TOYS_INTERFACE) == 0) return context;
    return nullptr;
}
}
extern "C" SF_EXPORT const SfNativePluginV1* sf_plugin_query_v1() {
    static const SfNativePluginV1 plugin{sizeof(SfNativePluginV1), SF_NATIVE_PLUGIN_ABI,
        "org.scifishell.parks-scene", create, start, stop, getInterface, destroy};
    return &plugin;
}
