#pragma once

#include "NativePluginApi.h"

#include <filesystem>
#include <string>

namespace sf {

// The caller must establish trust before loading; native code has full process privileges.
class NativePlugin {
public:
    NativePlugin() = default;
    ~NativePlugin();
    NativePlugin(const NativePlugin&) = delete;
    NativePlugin& operator=(const NativePlugin&) = delete;

    bool load(const std::filesystem::path& path, const std::string& expectedId, std::string& error);
    bool start(std::string& error);
    const void* getInterface(const char* id) const;
    void stop();
    void unload();
    bool running() const { return running_; }

private:
    void* library_ = nullptr;
    SfNativePluginV1 plugin_{};
    void* context_ = nullptr;
    bool running_ = false;
};

}
