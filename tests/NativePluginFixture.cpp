#include "NativePluginApi.h"

#ifdef _WIN32
#define SF_EXPORT __declspec(dllexport)
#else
#define SF_EXPORT __attribute__((visibility("default")))
#endif

namespace {
void* create() {
#ifdef FIXTURE_FAIL_CREATE
    return nullptr;
#else
    static int context;
    return &context;
#endif
}
int start(void*) {
#ifdef FIXTURE_FAIL_START
    return 0;
#else
    return 1;
#endif
}
void stop(void*) {}
const void* getInterface(void*, const char*) { return nullptr; }
void destroy(void*) {}
}

extern "C" SF_EXPORT const SfNativePluginV1* sf_plugin_query_v1() {
    static const SfNativePluginV1 plugin{
        sizeof(SfNativePluginV1),
#ifdef FIXTURE_BAD_ABI
        SF_NATIVE_PLUGIN_ABI + 1,
#else
        SF_NATIVE_PLUGIN_ABI,
#endif
        "test.fixture", create, start, stop, getInterface, destroy
    };
    return &plugin;
}
