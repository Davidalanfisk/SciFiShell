#include "NativePlugin.h"

#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace sf {
namespace {
void closeLibrary(void* library) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}
}

NativePlugin::~NativePlugin() { unload(); }

bool NativePlugin::load(const std::filesystem::path& path, const std::string& expectedId, std::string& error) {
    if (library_) { error = "Native plugin is already loaded"; return false; }
    if (!path.is_absolute() || expectedId.empty()) { error = "Native plugin requires an absolute path and an ID"; return false; }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)) || ec) {
        error = "Native plugin must be a regular file";
        return false;
    }
#ifdef _WIN32
    library_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library_) { error = "Native plugin load failed (Windows error " + std::to_string(GetLastError()) + ")"; return false; }
    auto query = reinterpret_cast<SfNativePluginQueryV1>(GetProcAddress(static_cast<HMODULE>(library_), "sf_plugin_query_v1"));
#else
    library_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library_) { error = std::string("Native plugin load failed: ") + dlerror(); return false; }
    auto query = reinterpret_cast<SfNativePluginQueryV1>(dlsym(library_, "sf_plugin_query_v1"));
#endif
    if (!query) {
        error = "Native plugin entry point is missing";
        unload();
        return false;
    }
    const SfNativePluginV1* descriptor = query();
    if (!descriptor || descriptor->size != sizeof(SfNativePluginV1) ||
        descriptor->abi != SF_NATIVE_PLUGIN_ABI || !descriptor->id ||
        expectedId != descriptor->id || !descriptor->create || !descriptor->start ||
        !descriptor->stop || !descriptor->get_interface || !descriptor->destroy) {
        error = "Native plugin ABI or identity mismatch";
        unload();
        return false;
    }
    plugin_ = *descriptor;
    context_ = plugin_.create();
    if (!context_) {
        error = "Native plugin initialization failed";
        unload();
        return false;
    }
    error.clear();
    return true;
}

bool NativePlugin::start(std::string& error) {
    if (!library_) { error = "Native plugin is not loaded"; return false; }
    if (running_) { error.clear(); return true; }
    if (!plugin_.start(context_)) { error = "Native plugin start failed"; return false; }
    running_ = true;
    error.clear();
    return true;
}

const void* NativePlugin::getInterface(const char* id) const {
    return running_ && id ? plugin_.get_interface(context_, id) : nullptr;
}

void NativePlugin::stop() {
    if (running_) { plugin_.stop(context_); running_ = false; }
}

void NativePlugin::unload() {
    if (!library_) return;
    stop();
    if (context_) plugin_.destroy(context_);
    context_ = nullptr;
    plugin_ = {};
    closeLibrary(library_);
    library_ = nullptr;
}
}
