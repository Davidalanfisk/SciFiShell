#include "PluginScript.h"
#include "PluginLimits.h"
#include "GameplayRay.h"
#include "GameplayStorage.h"

#include <cstdlib>
#include <ctime>
#include <cmath>
#include <array>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <random>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace sf {
namespace {
constexpr size_t kMemoryLimit = 1024 * 1024;
constexpr size_t kCompileMemoryLimit = 128 * 1024 * 1024;
constexpr int kInstructionLimit = 100000;
struct Memory { size_t used = 0; size_t limit = kMemoryLimit; };

void* limitedAlloc(void* ud, void* ptr, size_t oldSize, size_t newSize) {
    auto& memory = *static_cast<Memory*>(ud);
    if (!ptr) oldSize = 0;
    if (!newSize) {
        std::free(ptr);
        memory.used -= oldSize;
        return nullptr;
    }
    if (newSize > oldSize && newSize - oldSize > memory.limit - memory.used) return nullptr;
    void* next = std::realloc(ptr, newSize);
    if (next) memory.used = memory.used - oldSize + newSize;
    return next;
}

int loadSource(lua_State* state, Memory& memory, const std::string& source) {
    memory.limit = kCompileMemoryLimit;
    const int status = luaL_loadbufferx(state, source.data(), source.size(), "plugin", "t");
    if (status == LUA_OK) {
        // Discard compiler scratch before fixing the loaded-code baseline. Execution
        // gets only 1 MiB of additional allocations, never the compilation budget.
        lua_gc(state, LUA_GCCOLLECT);
        memory.limit = memory.used + std::min(kMemoryLimit, kCompileMemoryLimit - memory.used);
    }
    return status;
}

void instructionLimit(lua_State* state, lua_Debug*) {
    luaL_error(state, "instruction limit exceeded");
}

int unixTime(lua_State* state) {
    lua_pushinteger(state, static_cast<lua_Integer>(std::time(nullptr)));
    return 1;
}

int initialize(lua_State* state) {
    lua_pushcfunction(state, unixTime);
    lua_setglobal(state, "unix_time");
    return 0;
}

char lifecycleKey;
char eventCallbacksKey;

int storeLifecycle(lua_State* state) {
    for (const char* name : {"on_start", "on_tick", "on_input", "on_stop", "on_message"}) {
        lua_pushstring(state, name);
        lua_rawget(state, 1);
        if (!lua_isnil(state, -1) && !lua_isfunction(state, -1))
            return luaL_error(state, "%s must be a function", name);
        lua_pop(state, 1);
    }
    lua_pushvalue(state, 1);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &lifecycleKey);
    return 0;
}

struct LifecycleCall {
    const char* event;
    const char* control;
    double value;
    int arguments;
    const PluginScriptRuntime::MessageEvent* message = nullptr;
    size_t senderSize = 0;
};

void pushMessagePayload(lua_State* state, const PluginScriptRuntime::MessageEvent& event) {
    switch (event.type) {
        case PluginScriptRuntime::MessageEvent::Type::String:
            lua_pushlstring(state, event.payload.data(), event.payloadSize); break;
        case PluginScriptRuntime::MessageEvent::Type::Number:
            lua_pushnumber(state, event.number); break;
        case PluginScriptRuntime::MessageEvent::Type::Boolean:
            lua_pushboolean(state, event.boolean); break;
    }
}

// Runs under lua_pcall so table access and callback invocation remain protected.
int dispatchLifecycle(lua_State* state) {
    const auto& call = *static_cast<const LifecycleCall*>(lua_touserdata(state, 1));
    lua_rawgetp(state, LUA_REGISTRYINDEX, &lifecycleKey);
    lua_pushstring(state, call.event);
    lua_rawget(state, -2);
    if (lua_isnil(state, -1)) return 0;
    if (!lua_isfunction(state, -1)) return luaL_error(state, "%s must be a function", call.event);
    if (call.arguments == 3) {
        lua_pushlstring(state, call.control, call.senderSize);
        lua_pushlstring(state, call.message->topic.data(), call.message->topicSize);
        pushMessagePayload(state, *call.message);
    } else {
        if (call.arguments == 2) lua_pushstring(state, call.control);
        if (call.arguments) lua_pushnumber(state, call.value);
    }
    lua_call(state, call.arguments, 0);
    return 0;
}

int dispatchRegisteredEvent(lua_State* state) {
    const auto& event = *static_cast<const PluginScriptRuntime::MessageEvent*>(lua_touserdata(state, 1));
    lua_rawgetp(state, LUA_REGISTRYINDEX, &eventCallbacksKey);
    lua_pushlstring(state, event.topic.data(), event.topicSize);
    lua_rawget(state, -2);
    if (lua_isnil(state, -1)) return 0;
    pushMessagePayload(state, event);
    lua_call(state, 1, 0);
    return 0;
}

int initializeGameplay(lua_State* state) {
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);
    return 0;
}
}

struct PluginScriptRuntime::Impl {
    Memory memory;
    lua_State* state = nullptr;
    std::array<Box, 128> scene{};
    std::array<Model, 128> models{};
    std::array<GameplayModel, 8> meshes{};
    std::array<int, 8> meshStates{}; // 0 unused, 1 building, 2 uploaded
    int nextId = 1;
    AssetCounts assets{};
    std::vector<std::string> assetNames;
    WorldQueries world{};
    ViewQuery view{};
    LayoutQuery layout{};
    GameplayStorage* storage = nullptr;
    std::string storageError;
    int storageCalls = 0;
    double storageWriteTokens = 8;
    std::chrono::steady_clock::time_point storageWriteTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
    std::uint64_t frame = 0;
    double delta = 0;
    bool framePrepared = false;
    std::mt19937_64 randomEngine{};
    SoundBatch sounds{};
    SampleBatch samples{};
    MessageBatch messages{};
    bool messagingGranted = false;
    size_t eventCallbackCount = 0;
    int nextSampleVoice = 1;
    EngineSound engineSound{};
    HudText hud{};
    Impulse impulse{};
    ControlOffer controlOffer{};
    ControlPose controlPose{};
    bool controlGranted = false, controlActive = false;
    bool clickCapture = false, handlingClickPress = false;
    int worldCalls = 0;

    static Impl& self(lua_State* state) {
        return **static_cast<Impl**>(lua_getextraspace(state));
    }

    static int elapsedTime(lua_State* state) {
        const auto elapsed = std::chrono::steady_clock::now() - self(state).startedAt;
        lua_pushnumber(state, std::chrono::duration<double>(elapsed).count());
        return 1;
    }

    static int monotonicTime(lua_State* state) {
        lua_pushnumber(state, std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        return 1;
    }

    static int frameTime(lua_State* state) {
        lua_pushinteger(state, static_cast<lua_Integer>(self(state).frame));
        return 1;
    }

    static int deltaTime(lua_State* state) {
        lua_pushnumber(state, self(state).delta);
        return 1;
    }

    static int randomUnit(lua_State* state) {
        lua_pushnumber(state, static_cast<double>(self(state).randomEngine() >> 11) * 0x1.0p-53);
        return 1;
    }

    static int randomRange(lua_State* state) {
        const double lo = luaL_checknumber(state, 1), hi = luaL_checknumber(state, 2);
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi)
            return luaL_error(state, "invalid random range");
        const double r = static_cast<double>(self(state).randomEngine() >> 11) * 0x1.0p-53;
        const double result = std::lerp(lo, hi, r);
        if (!std::isfinite(result)) return luaL_error(state, "random range overflow");
        lua_pushnumber(state, result);
        return 1;
    }

    static int randomSeed(lua_State* state) {
        const double seed = luaL_checknumber(state, 1);
        if (!std::isfinite(seed)) return luaL_error(state, "invalid random seed");
        static_assert(sizeof(lua_Integer) == sizeof(seed));
        std::uint64_t bits;
        std::memcpy(&bits, &seed, sizeof(bits));
        self(state).randomEngine.seed(bits);
        return 0;
    }

    static int wallTime(lua_State* state) {
        const auto now = std::time(nullptr);
        if (now == static_cast<std::time_t>(-1)) return luaL_error(state, "system time unavailable");
        lua_pushinteger(state, static_cast<lua_Integer>(now));
        return 1;
    }

    static int initializeTime(lua_State* state) {
        lua_pushcfunction(state, elapsedTime); lua_setglobal(state, "time_elapsed");
        lua_pushcfunction(state, wallTime); lua_setglobal(state, "time_unix");
        lua_pushcfunction(state, monotonicTime); lua_setglobal(state, "time_now");
        lua_pushcfunction(state, elapsedTime); lua_setglobal(state, "time_since_start");
        lua_pushcfunction(state, frameTime); lua_setglobal(state, "time_frame");
        lua_pushcfunction(state, deltaTime); lua_setglobal(state, "time_delta");
        lua_pushcfunction(state, randomUnit); lua_setglobal(state, "random");
        lua_pushcfunction(state, randomRange); lua_setglobal(state, "random_range");
        lua_pushcfunction(state, randomSeed); lua_setglobal(state, "random_seed");
        return 0;
    }

    static int emitMessageImpl(lua_State* state, bool typed) {
        auto& self = Impl::self(state);
        size_t topicSize = 0, payloadSize = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const int type = lua_type(state, 2);
        if ((!typed && type != LUA_TSTRING) ||
            (typed && type != LUA_TSTRING && type != LUA_TNUMBER && type != LUA_TBOOLEAN))
            return luaL_error(state, "message payload must be a string, number, or boolean");
        const char* topic = luaL_checklstring(state, 1, &topicSize);
        const char* payload = type == LUA_TSTRING ? lua_tolstring(state, 2, &payloadSize) : nullptr;
        const double number = type == LUA_TNUMBER ? lua_tonumber(state, 2) : 0;
        if (type == LUA_TNUMBER && !std::isfinite(number)) return luaL_error(state, "invalid message number");
        if (topicSize == 0 || topicSize > 32 || payloadSize > 256)
            return luaL_error(state, "invalid message size");
        for (size_t i = 0; i < topicSize; ++i) {
            const char c = topic[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                return luaL_error(state, "invalid message topic");
        }
        if (self.messages.count == self.messages.events.size())
            return luaL_error(state, "message queue limit exceeded");
        auto& event = self.messages.events[self.messages.count++];
        std::memcpy(event.topic.data(), topic, topicSize);
        if (payloadSize) std::memcpy(event.payload.data(), payload, payloadSize);
        event.topicSize = topicSize;
        event.payloadSize = payloadSize;
        event.type = type == LUA_TNUMBER ? MessageEvent::Type::Number :
                     type == LUA_TBOOLEAN ? MessageEvent::Type::Boolean : MessageEvent::Type::String;
        event.number = number;
        event.boolean = type == LUA_TBOOLEAN && lua_toboolean(state, 2);
        return 0;
    }

    static int emitMessage(lua_State* state) { return emitMessageImpl(state, false); }
    static int sendEvent(lua_State* state) { return emitMessageImpl(state, true); }

    static int registerEvent(lua_State* state) {
        size_t length = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const char* name = luaL_checklstring(state, 1, &length);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        if (!length || length > 32) return luaL_error(state, "invalid message topic");
        for (size_t i = 0; i < length; ++i) {
            const char c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                return luaL_error(state, "invalid message topic");
        }
        lua_rawgetp(state, LUA_REGISTRYINDEX, &eventCallbacksKey);
        lua_pushvalue(state, 1); lua_rawget(state, -2);
        const bool exists = !lua_isnil(state, -1);
        lua_pop(state, 1);
        auto& self = Impl::self(state);
        if (!exists && self.eventCallbackCount >= 32) return luaL_error(state, "event callback limit exceeded");
        lua_pushvalue(state, 1); lua_pushvalue(state, 2); lua_rawset(state, -3);
        if (!exists) ++self.eventCallbackCount;
        return 0;
    }

    static int initializeMessaging(lua_State* state) {
        lua_newtable(state); lua_rawsetp(state, LUA_REGISTRYINDEX, &eventCallbacksKey);
        lua_pushcfunction(state, emitMessage); lua_setglobal(state, "message_emit");
        lua_pushcfunction(state, sendEvent); lua_setglobal(state, "send_event");
        lua_pushcfunction(state, registerEvent); lua_setglobal(state, "on_event");
        return 0;
    }

    static int createObject(lua_State* state, bool isModel, bool procedural = false) {
        auto& self = Impl::self(state);
        lua_Integer model = 0, texture = 0;
        if (isModel) {
            model = luaL_checkinteger(state, 1);
            const bool defaults = procedural && lua_gettop(state) == 1;
            texture = defaults ? 0 : luaL_checkinteger(state, 2);
            if (model < 1 || (procedural ? model > static_cast<lua_Integer>(self.meshes.size()) ||
                                                self.meshStates[static_cast<size_t>(model - 1)] != 2 :
                                           model > self.assets.models) ||
                texture < 0 || texture > self.assets.textures)
                return luaL_error(state, "unknown model or texture asset");
            if (procedural) model = -model;
        }
        std::array<double, 9> values{};
        for (int i = 0; i < 9; ++i) {
            values[i] = procedural && lua_gettop(state) == 1 ? (i < 3 ? 0 : 1) :
                        luaL_checknumber(state, i + 1 + (isModel ? 2 : 0));
            if (!std::isfinite(values[i])) return luaL_error(state, "box values must be finite");
            if (i < 3 && std::abs(values[i]) > 10000) return luaL_error(state, "box position out of range");
            if (i >= 3 && i < 6 && (values[i] <= 0 || values[i] > 1000))
                return luaL_error(state, "box size out of range");
            if (i >= 6 && (values[i] < 0 || values[i] > 1))
                return luaL_error(state, "box colour out of range");
        }
        int count = 0;
        for (const auto& box : self.scene) if (box.id) ++count;
        for (const auto& item : self.models) if (item.transform.id) ++count;
        if (count >= 128) return luaL_error(state, "scene object limit exceeded");
        if (self.nextId == 0x7fffffff) return luaL_error(state, "scene handle limit reached");
        Box value{self.nextId++, values[0], values[1], values[2], values[3], values[4], values[5],
                  values[6], values[7], values[8]};
        if (isModel) {
            for (auto& item : self.models) if (!item.transform.id) {
                item = {value, static_cast<int>(model), static_cast<int>(texture)};
                lua_pushinteger(state, value.id);
                return 1;
            }
        } else {
            for (auto& box : self.scene) if (!box.id) {
                box = value;
                lua_pushinteger(state, value.id);
                return 1;
            }
        }
        return luaL_error(state, "scene object limit exceeded");
    }

    static int createBox(lua_State* state) { return createObject(state, false); }
    static int createModel(lua_State* state) { return createObject(state, true); }
    static int createMeshObject(lua_State* state) { return createObject(state, true, true); }

    static int createMesh(lua_State* state) {
        auto& self = Impl::self(state);
        size_t slot = 0;
        while (slot < self.meshes.size() && self.meshStates[slot] != 0) ++slot;
        if (slot == self.meshes.size()) return luaL_error(state, "mesh limit exceeded");
        if (lua_gettop(state) == 0) {
            self.meshStates[slot] = 1;
            lua_pushinteger(state, static_cast<lua_Integer>(slot + 1));
            return 1;
        }
        luaL_checktype(state, 1, LUA_TTABLE);
        luaL_checktype(state, 2, LUA_TTABLE);
        const size_t vertexValues = lua_rawlen(state, 1), indexCount = lua_rawlen(state, 2);
        if (!vertexValues || vertexValues % 8 || vertexValues > 1024 * 8 ||
            !indexCount || indexCount % 3 || indexCount > 6144)
            return luaL_error(state, "invalid mesh size");
        std::array<float, 1024 * 8> vertices{};
        std::array<std::uint32_t, 6144> indices{};
        for (size_t i = 0; i < vertexValues; ++i) {
            lua_rawgeti(state, 1, static_cast<lua_Integer>(i + 1));
            int numeric = 0;
            const bool numberType = lua_type(state, -1) == LUA_TNUMBER;
            const double value = lua_tonumberx(state, -1, &numeric);
            lua_pop(state, 1);
            if (!numberType || !numeric || !std::isfinite(value) || std::abs(value) > 10000)
                return luaL_error(state, "invalid mesh vertex");
            vertices[i] = static_cast<float>(value);
        }
        const size_t vertexCount = vertexValues / 8;
        for (size_t i = 0; i < indexCount; ++i) {
            lua_rawgeti(state, 2, static_cast<lua_Integer>(i + 1));
            int numeric = 0;
            const bool numberType = lua_type(state, -1) == LUA_TNUMBER;
            const lua_Integer value = lua_tointegerx(state, -1, &numeric);
            lua_pop(state, 1);
            if (!numberType || !numeric || value < 1 || static_cast<size_t>(value) > vertexCount)
                return luaL_error(state, "invalid mesh index");
            indices[i] = static_cast<std::uint32_t>(value - 1);
        }
        bool allocated = false;
        try {
            self.meshes[slot].vertices.assign(vertices.begin(), vertices.begin() + vertexValues);
            self.meshes[slot].indices.assign(indices.begin(), indices.begin() + indexCount);
            allocated = true;
        } catch (const std::bad_alloc&) {
            self.meshes[slot] = {};
        }
        if (!allocated) return luaL_error(state, "cannot allocate mesh");
        self.meshStates[slot] = 2;
        lua_pushinteger(state, static_cast<lua_Integer>(slot + 1));
        return 1;
    }

    static int meshBuilderSlot(lua_State* state) {
        const lua_Integer handle = luaL_checkinteger(state, 1);
        const auto& self = Impl::self(state);
        if (handle < 1 || handle > static_cast<lua_Integer>(self.meshes.size()) ||
            self.meshStates[static_cast<size_t>(handle - 1)] != 1)
            return luaL_error(state, "unknown or uploaded mesh builder");
        return static_cast<int>(handle - 1);
    }

    static int meshVertex(lua_State* state) {
        const int slot = meshBuilderSlot(state);
        std::array<float, 8> values{0, 0, 0, 0, 0, 1, 0, 0};
        for (int i = 0; i < 3; ++i) {
            const double n = luaL_checknumber(state, i + 2);
            if (!std::isfinite(n) || std::abs(n) > 10000) return luaL_error(state, "invalid mesh vertex");
            values[i] = static_cast<float>(n);
        }
        auto& vertices = self(state).meshes[slot].vertices;
        if (vertices.size() >= 1024 * 8) return luaL_error(state, "mesh vertex limit exceeded");
        bool ok = false;
        try { vertices.insert(vertices.end(), values.begin(), values.end()); ok = true; }
        catch (const std::bad_alloc&) {}
        if (!ok) return luaL_error(state, "cannot allocate mesh");
        lua_pushinteger(state, static_cast<lua_Integer>(vertices.size() / 8));
        return 1;
    }

    static int meshAttribute(lua_State* state, int offset, int count) {
        const int slot = meshBuilderSlot(state);
        std::array<float, 3> values{};
        for (int i = 0; i < count; ++i) {
            const double n = luaL_checknumber(state, i + 2);
            if (!std::isfinite(n) || std::abs(n) > 10000) return luaL_error(state, "invalid mesh attribute");
            values[i] = static_cast<float>(n);
        }
        auto& vertices = self(state).meshes[slot].vertices;
        if (vertices.empty()) return luaL_error(state, "mesh has no vertex");
        for (int i = 0; i < count; ++i) vertices[vertices.size() - 8 + offset + i] = values[i];
        return 0;
    }
    static int meshNormal(lua_State* state) { return meshAttribute(state, 3, 3); }
    static int meshUv(lua_State* state) { return meshAttribute(state, 6, 2); }

    static int meshTriangle(lua_State* state) {
        const int slot = meshBuilderSlot(state);
        auto& mesh = self(state).meshes[slot];
        std::array<std::uint32_t, 3> indices{};
        for (int i = 0; i < 3; ++i) {
            const lua_Integer n = luaL_checkinteger(state, i + 2);
            if (n < 1 || n > static_cast<lua_Integer>(mesh.vertices.size() / 8))
                return luaL_error(state, "invalid mesh index");
            indices[i] = static_cast<std::uint32_t>(n - 1);
        }
        if (mesh.indices.size() > 6144 - 3) return luaL_error(state, "mesh index limit exceeded");
        bool ok = false;
        try { mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end()); ok = true; }
        catch (const std::bad_alloc&) {}
        if (!ok) return luaL_error(state, "cannot allocate mesh");
        return 0;
    }

    static int meshUpload(lua_State* state) {
        const int slot = meshBuilderSlot(state);
        auto& self = Impl::self(state);
        if (self.meshes[slot].vertices.empty() || self.meshes[slot].indices.empty())
            return luaL_error(state, "mesh requires vertices and triangles");
        self.meshStates[slot] = 2;
        lua_pushinteger(state, slot + 1);
        return 1;
    }

    static int findAsset(lua_State* state, bool model) {
        size_t length = 0;
        const char* name = luaL_checklstring(state, 1, &length);
        const auto& names = model ? self(state).assets.modelNames : self(state).assets.textureNames;
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].size() == length && std::memcmp(names[i].data(), name, length) == 0) {
                lua_pushinteger(state, static_cast<lua_Integer>(i + 1));
                return 1;
            }
        }
        return luaL_error(state, "unknown asset");
    }

    static int findModel(lua_State* state) { return findAsset(state, true); }
    static int findTexture(lua_State* state) { return findAsset(state, false); }

    static int assetExists(lua_State* state) {
        size_t length = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const char* name = luaL_checklstring(state, 1, &length);
        if (!length || length > 87) return luaL_error(state, "invalid asset name");
        const auto& names = self(state).assetNames;
        bool found = false;
        for (const auto& entry : names)
            if (entry.size() == length && std::memcmp(entry.data(), name, length) == 0) { found = true; break; }
        lua_pushboolean(state, found);
        return 1;
    }

    static int listAssets(lua_State* state) {
        size_t length = 0;
        const char* prefix = "";
        if (!lua_isnone(state, 1)) {
            luaL_checktype(state, 1, LUA_TSTRING);
            prefix = luaL_checklstring(state, 1, &length);
            if (length > 87) return luaL_error(state, "invalid asset prefix");
        }
        lua_newtable(state);
        int index = 1;
        for (const auto& name : self(state).assetNames) {
            if (name.size() >= length && std::memcmp(name.data(), prefix, length) == 0) {
                lua_pushlstring(state, name.data(), name.size());
                lua_rawseti(state, -2, index++);
            }
        }
        return 1;
    }

    static bool globMatch(std::string_view pattern, std::string_view path) {
        size_t p = 0, s = 0, star = std::string_view::npos, retry = 0;
        while (s < path.size()) {
            if (p < pattern.size() && (pattern[p] == path[s] || (pattern[p] == '?' && path[s] != '/'))) {
                ++p; ++s;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                retry = s;
            } else if (star != std::string_view::npos && retry < path.size() && path[retry] != '/') {
                p = star + 1;
                s = ++retry;
            } else return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }

    static int findAssets(lua_State* state) {
        size_t length = 0;
        const char* text = "";
        if (!lua_isnone(state, 1)) {
            luaL_checktype(state, 1, LUA_TSTRING);
            text = luaL_checklstring(state, 1, &length);
        }
        if (length > 87) return luaL_error(state, "invalid asset pattern");
        std::string_view pattern(text, length);
        const bool full = pattern.substr(0, 7) == "assets/";
        const bool glob = pattern.find_first_of("*?") != std::string_view::npos;
        lua_newtable(state);
        int index = 1;
        for (const auto& name : self(state).assetNames) {
            std::string_view candidate(name);
            if (!full && candidate.substr(0, 7) == "assets/") candidate.remove_prefix(7);
            if (glob ? globMatch(pattern, candidate) : candidate.substr(0, length) == pattern) {
                lua_pushlstring(state, name.data(), name.size());
                lua_rawseti(state, -2, index++);
            }
        }
        return 1;
    }

    static int removeBox(lua_State* state) {
        auto& self = Impl::self(state);
        const lua_Integer id = luaL_checkinteger(state, 1);
        for (auto& box : self.scene) {
            if (box.id && box.id == id) { box = {}; return 0; }
        }
        for (auto& item : self.models) {
            if (item.transform.id && item.transform.id == id) { item = {}; return 0; }
        }
        return luaL_error(state, "unknown scene object");
    }

    enum class BoxProperty { Position, Rotation, Size, Colour };

    static int updateBox(lua_State* state, BoxProperty property) {
        auto& self = Impl::self(state);
        const lua_Integer id = luaL_checkinteger(state, 1);
        std::array<double, 3> values{};
        for (int i = 0; i < 3; ++i) {
            values[i] = luaL_checknumber(state, i + 2);
            if (!std::isfinite(values[i])) return luaL_error(state, "box values must be finite");
            if ((property == BoxProperty::Position || property == BoxProperty::Rotation) &&
                std::abs(values[i]) > 10000) return luaL_error(state, "box transform out of range");
            if (property == BoxProperty::Size && (values[i] <= 0 || values[i] > 1000))
                return luaL_error(state, "box size out of range");
            if (property == BoxProperty::Colour && (values[i] < 0 || values[i] > 1))
                return luaL_error(state, "box colour out of range");
        }
        auto update = [&](Box& box) {
            if (!box.id || box.id != id) return false;
            switch (property) {
                case BoxProperty::Position: box.x = values[0]; box.y = values[1]; box.z = values[2]; break;
                case BoxProperty::Rotation: box.rotateX = values[0]; box.rotateY = values[1]; box.rotateZ = values[2]; break;
                case BoxProperty::Size: box.width = values[0]; box.height = values[1]; box.depth = values[2]; break;
                case BoxProperty::Colour: box.red = values[0]; box.green = values[1]; box.blue = values[2]; break;
            }
            return true;
        };
        for (auto& box : self.scene) if (update(box)) return 0;
        for (auto& item : self.models) if (update(item.transform)) return 0;
        return luaL_error(state, "unknown scene object");
    }

    static int moveBox(lua_State* state) { return updateBox(state, BoxProperty::Position); }
    static int rotateBox(lua_State* state) { return updateBox(state, BoxProperty::Rotation); }
    static int resizeBox(lua_State* state) { return updateBox(state, BoxProperty::Size); }
    static int colourBox(lua_State* state) { return updateBox(state, BoxProperty::Colour); }

    static int setInteractive(lua_State* state) {
        auto& self = Impl::self(state);
        const lua_Integer id = luaL_checkinteger(state, 1);
        luaL_checktype(state, 2, LUA_TBOOLEAN);
        const bool enabled = lua_toboolean(state, 2) != 0;
        for (auto& box : self.scene) if (box.id && box.id == id) { box.interactive = enabled; return 0; }
        for (auto& item : self.models) if (item.transform.id && item.transform.id == id) {
            item.transform.interactive = enabled;
            return 0;
        }
        return luaL_error(state, "unknown scene object");
    }

    static int captureClicks(lua_State* state) {
        auto& self = Impl::self(state);
        luaL_checktype(state, 1, LUA_TBOOLEAN);
        const bool enabled = lua_toboolean(state, 1) != 0;
        if (enabled && !self.clickCapture && !self.handlingClickPress)
            return luaL_error(state, "click capture requires a routed click press");
        self.clickCapture = enabled;
        return 0;
    }

    static int hitObject(lua_State* state) {
        auto& self = Impl::self(state);
        const lua_Integer id = luaL_checkinteger(state, 1);
        std::array<double, 7> v{};
        for (int i = 0; i < 7; ++i) {
            v[i] = luaL_checknumber(state, i + 2);
            if (!std::isfinite(v[i])) return luaL_error(state, "scene ray values must be finite");
        }
        for (int i = 0; i < 3; ++i)
            if (std::abs(v[i]) > 10000) return luaL_error(state, "scene ray origin out of range");
        for (int i = 3; i < 6; ++i)
            if (std::abs(v[i]) > 1.001) return luaL_error(state, "scene ray direction out of range");
        if (v[6] < 0 || v[6] > 100) return luaL_error(state, "scene ray distance out of range");
        const double len = std::sqrt(v[3]*v[3] + v[4]*v[4] + v[5]*v[5]);
        if (len < 0.000001) return luaL_error(state, "scene ray direction is zero");
        if (++self.worldCalls > 64) return luaL_error(state, "host query limit exceeded");
        const Box* object = nullptr;
        for (const auto& box : self.scene) if (box.id && box.id == id) { object = &box; break; }
        if (!object) for (const auto& model : self.models)
            if (model.transform.id && model.transform.id == id) { object = &model.transform; break; }
        if (!object) return luaL_error(state, "unknown scene object");
        double distance = 0;
        if (!gameplayRayHit(*object, v.data(), v.data() + 3, v[6], distance)) lua_pushnil(state);
        else lua_pushnumber(state, distance);
        return 1;
    }

    static int worldQuery(lua_State* state, bool resolve) {
        auto& self = Impl::self(state);
        std::array<double, 5> v{};
        for (int i = 0; i < 5; ++i) {
            v[i] = luaL_checknumber(state, i + 1);
            if (!std::isfinite(v[i]) || std::abs(v[i]) > 10000)
                return luaL_error(state, "world query value out of range");
        }
        if (v[3] < 0 || v[3] > 100 || v[4] < 0 || v[4] > 100)
            return luaL_error(state, "world query radius or eye height out of range");
        if (++self.worldCalls > 64) return luaL_error(state, "world query limit exceeded");
        if (resolve) {
            if (!self.world.resolve(self.world.context, &v[0], &v[1], &v[2], v[3], v[4]))
                return luaL_error(state, "world collision query failed");
            for (int i = 0; i < 3; ++i) {
                if (!std::isfinite(v[i]) || std::abs(v[i]) > 10000)
                    return luaL_error(state, "world collision returned invalid position");
                lua_pushnumber(state, v[i]);
            }
            return 3;
        }
        double result = 0;
        if (!self.world.floor(self.world.context, v[0], v[1], v[2], v[3], v[4], &result) ||
            !std::isfinite(result) || std::abs(result) > 10000)
            return luaL_error(state, "world floor query failed");
        lua_pushnumber(state, result);
        return 1;
    }

    static int floorQuery(lua_State* state) { return worldQuery(state, false); }
    static int resolveQuery(lua_State* state) { return worldQuery(state, true); }
    static int trackClearance(lua_State* state) {
        auto& self = Impl::self(state);
        const double x = luaL_checknumber(state, 1), z = luaL_checknumber(state, 2);
        const double radius = luaL_checknumber(state, 3);
        if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius) ||
            std::abs(x) > 10000 || std::abs(z) > 10000 || radius < 0 || radius > 100)
            return luaL_error(state, "track clearance query out of range");
        if (++self.worldCalls > 64) return luaL_error(state, "world query limit exceeded");
        double result = 0;
        if (!self.world.trackClearance(self.world.context, x, z, radius, &result) ||
            !std::isfinite(result) || result < 0 || result > 10000)
            return luaL_error(state, "track clearance query failed");
        lua_pushnumber(state, result);
        return 1;
    }
    static int worldAvailable(lua_State* state) {
        auto& self = Impl::self(state);
        if (++self.worldCalls > 64) return luaL_error(state, "world query limit exceeded");
        lua_pushboolean(state, self.world.available(self.world.context));
        return 1;
    }

    static int initializeWorld(lua_State* state) {
        lua_pushcfunction(state, worldAvailable);
        lua_setglobal(state, "world_available");
        lua_pushcfunction(state, floorQuery);
        lua_setglobal(state, "world_floor");
        lua_pushcfunction(state, resolveQuery);
        lua_setglobal(state, "world_resolve");
        if (self(state).world.trackClearance) {
            lua_pushcfunction(state, trackClearance);
            lua_setglobal(state, "world_track_clearance");
        }
        return 0;
    }

    static int viewPose(lua_State* state) {
        auto& self = Impl::self(state);
        if (++self.worldCalls > 64) return luaL_error(state, "host query limit exceeded");
        double pose[6]{};
        if (!self.view.pose(self.view.context, pose)) {
            lua_pushnil(state);
            return 1;
        }
        for (int i = 0; i < 6; ++i) {
            if (!std::isfinite(pose[i]) || std::abs(pose[i]) > (i < 3 ? 10000 : 1.001))
                return luaL_error(state, "host returned invalid view pose");
            lua_pushnumber(state, pose[i]);
        }
        return 6;
    }

    static int initializeView(lua_State* state) {
        lua_pushcfunction(state, viewPose);
        lua_setglobal(state, "view_pose");
        return 0;
    }

    static int cityCell(lua_State* state) {
        auto& self = Impl::self(state);
        const double x = luaL_checknumber(state, 1), z = luaL_checknumber(state, 2);
        if (!std::isfinite(x) || !std::isfinite(z) || std::abs(x) > 10000 || std::abs(z) > 10000)
            return luaL_error(state, "city cell position out of range");
        if (++self.worldCalls > 64) return luaL_error(state, "host query limit exceeded");
        double result[5]{};
        const int status = self.layout.cell(self.layout.context, x, z, result);
        if (status < 0 || status > 1) return luaL_error(state, "city cell query failed");
        if (!status) { lua_pushnil(state); return 1; }
        for (int i = 0; i < 4; ++i) {
            if (!std::isfinite(result[i]) || std::abs(result[i]) > 10000 || (i == 3 && result[i] <= 0))
                return luaL_error(state, "host returned invalid city cell");
            lua_pushnumber(state, result[i]);
        }
        if (result[4] != 0 && result[4] != 1) return luaL_error(state, "host returned invalid city occupancy");
        lua_pushboolean(state, result[4] != 0);
        return 5;
    }

    static int initializeLayout(lua_State* state) {
        lua_pushcfunction(state, cityCell);
        lua_setglobal(state, "city_cell");
        return 0;
    }

    static int playSound(lua_State* state) {
        auto& self = Impl::self(state);
        size_t length = 0;
        const char* name = luaL_checklstring(state, 1, &length);
        const char* names[] = {"pickup", "throw", "boom", "duck_pop", "car_summon", "rail_on"};
        int index = -1;
        for (int i = 0; i < 6; ++i)
            if (std::strlen(names[i]) == length && std::memcmp(name, names[i], length) == 0) index = i;
        if (index < 0) return luaL_error(state, "unknown sound effect");
        const double volume = luaL_checknumber(state, 2);
        if (!std::isfinite(volume) || volume < 0 || volume > 1)
            return luaL_error(state, "sound volume out of range");
        if (self.sounds.count == self.sounds.events.size())
            return luaL_error(state, "sound event limit exceeded");
        self.sounds.events[self.sounds.count++] = {static_cast<Sound>(index), static_cast<float>(volume)};
        return 0;
    }

    static int setEngineSound(lua_State* state) {
        auto& self = Impl::self(state);
        luaL_checktype(state, 1, LUA_TBOOLEAN);
        const double speed = luaL_checknumber(state, 2);
        luaL_checktype(state, 3, LUA_TBOOLEAN);
        if (!std::isfinite(speed) || speed < 0 || speed > 110)
            return luaL_error(state, "engine speed out of range");
        self.engineSound = {true, lua_toboolean(state, 1) != 0, lua_toboolean(state, 3) != 0,
                            static_cast<float>(speed)};
        return 0;
    }

    static int playSample(lua_State* state) {
        auto& self = Impl::self(state);
        size_t length = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const char* name = luaL_checklstring(state, 1, &length);
        const double volume = luaL_checknumber(state, 2);
        luaL_checktype(state, 3, LUA_TBOOLEAN);
        if (!std::isfinite(volume) || volume < 0 || volume > 1)
            return luaL_error(state, "sample volume out of range");
        int asset = -1;
        for (size_t i = 0; i < self.assets.audioNames.size(); ++i)
            if (self.assets.audioNames[i].size() == length &&
                std::memcmp(name, self.assets.audioNames[i].data(), length) == 0) asset = static_cast<int>(i);
        if (asset < 0) return luaL_error(state, "unknown audio asset");
        if (self.samples.count == self.samples.events.size()) return luaL_error(state, "sample event limit exceeded");
        if (self.nextSampleVoice == 0x7fffffff) return luaL_error(state, "sample voice handle limit exceeded");
        const int voice = self.nextSampleVoice++;
        self.samples.events[self.samples.count++] = {voice, asset, static_cast<float>(volume), false,
                                                     lua_toboolean(state, 3) != 0};
        lua_pushinteger(state, voice);
        return 1;
    }

    static int stopSample(lua_State* state) {
        auto& self = Impl::self(state);
        const lua_Integer voice = luaL_checkinteger(state, 1);
        if (voice <= 0 || voice >= self.nextSampleVoice) return luaL_error(state, "unknown sample voice");
        if (self.samples.count == self.samples.events.size()) return luaL_error(state, "sample event limit exceeded");
        self.samples.events[self.samples.count++] = {static_cast<int>(voice), 0, 0, true, false};
        return 0;
    }

    static int soundAsset(lua_State* state) {
        size_t length = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const char* text = luaL_checklstring(state, 1, &length);
        if (!length || length > 87) return luaL_error(state, "invalid audio asset name");
        const std::string_view path(text, length);
        const auto& names = self(state).assets.audioNames;
        for (size_t i = 0; i < names.size(); ++i) {
            std::string_view name(names[i]);
            if (path.substr(0, 7) != "assets/" && name.substr(0, 7) == "assets/") name.remove_prefix(7);
            if (name == path) {
                lua_pushinteger(state, static_cast<lua_Integer>(i + 1));
                return 1;
            }
        }
        return luaL_error(state, "unknown audio asset");
    }

    static int playSoundAsset(lua_State* state) {
        auto& self = Impl::self(state);
        const lua_Integer handle = luaL_checkinteger(state, 1);
        if (handle < 1 || handle > static_cast<lua_Integer>(self.assets.audioNames.size()))
            return luaL_error(state, "unknown audio asset");
        if (self.samples.count == self.samples.events.size()) return luaL_error(state, "sample event limit exceeded");
        if (self.nextSampleVoice == 0x7fffffff) return luaL_error(state, "sample voice handle limit exceeded");
        const int voice = self.nextSampleVoice++;
        self.samples.events[self.samples.count++] = {voice, static_cast<int>(handle - 1), 1.0f, false, false};
        lua_pushinteger(state, voice);
        return 1;
    }

    static int loopSample(lua_State* state) {
        auto& self = Impl::self(state);
        const lua_Integer voice = luaL_checkinteger(state, 1);
        if (voice <= 0 || voice >= self.nextSampleVoice) return luaL_error(state, "unknown sample voice");
        for (size_t i = self.samples.count; i > 0; --i) {
            auto& event = self.samples.events[i - 1];
            if (event.voice == voice) {
                if (event.stop) return luaL_error(state, "sample voice is stopped");
                if (!event.loopUpdate) { event.loop = true; return 0; }
                return 0;
            }
        }
        if (self.samples.count == self.samples.events.size()) return luaL_error(state, "sample event limit exceeded");
        self.samples.events[self.samples.count++] = {static_cast<int>(voice), 0, 0, false, true, true};
        return 0;
    }

    static int initializeSound(lua_State* state) {
        lua_pushcfunction(state, playSound);
        lua_setglobal(state, "sound_play");
        lua_pushcfunction(state, setEngineSound);
        lua_setglobal(state, "sound_engine");
        lua_pushcfunction(state, playSample);
        lua_setglobal(state, "sound_sample");
        lua_pushcfunction(state, stopSample);
        lua_setglobal(state, "sound_stop");
        lua_pushcfunction(state, soundAsset); lua_setglobal(state, "asset_sound");
        lua_pushcfunction(state, playSoundAsset); lua_setglobal(state, "sound_play_asset");
        lua_pushcfunction(state, loopSample); lua_setglobal(state, "sound_loop");
        return 0;
    }

    static int setHudText(lua_State* state) {
        auto& self = Impl::self(state);
        luaL_checktype(state, 1, LUA_TSTRING);
        size_t length = 0;
        const char* text = luaL_checklstring(state, 1, &length);
        if (length > 32) return luaL_error(state, "HUD text exceeds 32 characters");
        for (size_t i = 0; i < length; ++i)
            if (static_cast<unsigned char>(text[i]) < 32 || static_cast<unsigned char>(text[i]) > 126)
                return luaL_error(state, "HUD text must be printable ASCII");
        if (std::strlen(self.hud.text.data()) == length && std::memcmp(self.hud.text.data(), text, length) == 0)
            return 0;
        if (self.hud.revision == UINT64_MAX) return luaL_error(state, "HUD update limit reached");
        std::memcpy(self.hud.text.data(), text, length);
        self.hud.text[length] = '\0';
        ++self.hud.revision;
        return 0;
    }

    static int initializeHud(lua_State* state) {
        lua_pushcfunction(state, setHudText);
        lua_setglobal(state, "hud_set");
        return 0;
    }

    static int playerImpulse(lua_State* state) {
        auto& self = Impl::self(state);
        const double x = luaL_checknumber(state, 1), y = luaL_checknumber(state, 2), z = luaL_checknumber(state, 3);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            std::abs(x) > 12 || y < 0 || y > 8 || std::abs(z) > 12 ||
            x*x + z*z > 144)
            return luaL_error(state, "player impulse out of range");
        if (self.impulse.pending) return luaL_error(state, "player impulse queue is full");
        self.impulse = {x, y, z, true};
        return 0;
    }

    static int initializeImpulse(lua_State* state) {
        lua_pushcfunction(state, playerImpulse);
        lua_setglobal(state, "player_impulse");
        return 0;
    }

    static int offerControl(lua_State* state) {
        auto& self = Impl::self(state);
        if (lua_isnil(state, 1)) {
            self.controlOffer = {};
            return 0;
        }
        double values[3];
        for (int i = 0; i < 3; ++i) {
            values[i] = luaL_checknumber(state, i + 1);
            if (!std::isfinite(values[i]) || std::abs(values[i]) > 10000)
                return luaL_error(state, "control offer position out of range");
        }
        self.controlOffer = {values[0], values[1], values[2], true};
        return 0;
    }

    static int proposeControlPose(lua_State* state) {
        auto& self = Impl::self(state);
        if (!self.controlActive) return luaL_error(state, "control is not active");
        if (self.controlPose.pending) return luaL_error(state, "control pose queue is full");
        double values[5];
        for (int i = 0; i < 5; ++i) {
            values[i] = luaL_checknumber(state, i + 1);
            if (!std::isfinite(values[i]) || std::abs(values[i]) > 10000)
                return luaL_error(state, "control pose out of range");
        }
        if (std::abs(values[4]) > 1.4) return luaL_error(state, "control pitch out of range");
        self.controlPose = {values[0], values[1], values[2], values[3], values[4], true};
        return 0;
    }

    static int initializeControl(lua_State* state) {
        lua_pushcfunction(state, offerControl);
        lua_setglobal(state, "control_offer");
        lua_pushcfunction(state, proposeControlPose);
        lua_setglobal(state, "control_pose");
        return 0;
    }

    static int storageGet(lua_State* state) {
        size_t length = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const char* key = luaL_checklstring(state, 1, &length);
        auto& self = Impl::self(state);
        if (++self.storageCalls > 64) return luaL_error(state, "Storage query limit exceeded");
        const GameplayStorage::Value* value = nullptr;
        bool ok = false;
        try { ok = self.storage->get(std::string(key, length), value, self.storageError); }
        catch (const std::bad_alloc&) { self.storageError = "Cannot allocate plugin storage query"; }
        if (!ok) return luaL_error(state, "%s", self.storageError.c_str());
        if (!value) lua_pushnil(state);
        else if (const auto* text = std::get_if<std::string>(value)) lua_pushlstring(state, text->data(), text->size());
        else if (const auto* number = std::get_if<double>(value)) lua_pushnumber(state, *number);
        else lua_pushboolean(state, std::get<bool>(*value));
        return 1;
    }

    static int storageExists(lua_State* state) {
        size_t length = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const char* key = luaL_checklstring(state, 1, &length);
        auto& self = Impl::self(state);
        if (++self.storageCalls > 64) return luaL_error(state, "Storage query limit exceeded");
        const GameplayStorage::Value* value = nullptr;
        bool ok = false;
        try { ok = self.storage->get(std::string(key, length), value, self.storageError); }
        catch (const std::bad_alloc&) { self.storageError = "Cannot allocate plugin storage query"; }
        if (!ok) return luaL_error(state, "%s", self.storageError.c_str());
        lua_pushboolean(state, value != nullptr);
        return 1;
    }

    bool allowStorageWrite() {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - storageWriteTime).count();
        storageWriteTime = now;
        storageWriteTokens = std::min(8.0, storageWriteTokens + seconds * 4.0);
        if (++storageCalls > 64 || storageWriteTokens < 1) return false;
        storageWriteTokens -= 1;
        return true;
    }

    static int storageSet(lua_State* state) {
        size_t length = 0, textLength = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const char* key = luaL_checklstring(state, 1, &length);
        const int type = lua_type(state, 2);
        if (type != LUA_TSTRING && type != LUA_TNUMBER && type != LUA_TBOOLEAN)
            return luaL_error(state, "Storage values must be strings, numbers, or booleans");
        const char* text = type == LUA_TSTRING ? lua_tolstring(state, 2, &textLength) : nullptr;
        auto& self = Impl::self(state);
        if (!self.allowStorageWrite()) return luaL_error(state, "Storage write limit exceeded");
        bool ok = false;
        try {
            GameplayStorage::Value value;
            if (type == LUA_TSTRING) value = std::string(text, textLength);
            else if (type == LUA_TNUMBER) value = static_cast<double>(lua_tonumber(state, 2));
            else value = lua_toboolean(state, 2) != 0;
            ok = self.storage->set(std::string(key, length), std::move(value), self.storageError);
        } catch (const std::bad_alloc&) { self.storageError = "Cannot allocate plugin storage value"; }
        if (!ok) return luaL_error(state, "%s", self.storageError.c_str());
        return 0;
    }

    static int storageDelete(lua_State* state) {
        size_t length = 0;
        luaL_checktype(state, 1, LUA_TSTRING);
        const char* key = luaL_checklstring(state, 1, &length);
        auto& self = Impl::self(state);
        if (!self.allowStorageWrite()) return luaL_error(state, "Storage write limit exceeded");
        bool ok = false;
        try { ok = self.storage->erase(std::string(key, length), self.storageError); }
        catch (const std::bad_alloc&) { self.storageError = "Cannot allocate plugin storage deletion"; }
        if (!ok) return luaL_error(state, "%s", self.storageError.c_str());
        return 0;
    }

    static int initializeStorage(lua_State* state) {
        lua_pushcfunction(state, storageGet); lua_setglobal(state, "storage_get");
        lua_pushcfunction(state, storageSet); lua_setglobal(state, "storage_set");
        lua_pushcfunction(state, storageDelete); lua_setglobal(state, "storage_delete");
        lua_pushcfunction(state, storageExists); lua_setglobal(state, "storage_exists");
        return 0;
    }

    static int initializeScene(lua_State* state) {
        lua_pushcfunction(state, assetExists);
        lua_setglobal(state, "asset_exists");
        lua_pushcfunction(state, listAssets);
        lua_setglobal(state, "asset_list");
        lua_pushcfunction(state, findAssets);
        lua_setglobal(state, "asset_find");
        lua_pushcfunction(state, findModel);
        lua_setglobal(state, "asset_model");
        lua_pushcfunction(state, findTexture);
        lua_setglobal(state, "asset_texture");
        lua_pushcfunction(state, createBox);
        lua_setglobal(state, "scene_box");
        lua_pushcfunction(state, createModel);
        lua_setglobal(state, "scene_model");
        lua_pushcfunction(state, createMesh);
        lua_setglobal(state, "mesh_create");
        lua_pushcfunction(state, meshVertex); lua_setglobal(state, "mesh_vertex");
        lua_pushcfunction(state, meshNormal); lua_setglobal(state, "mesh_normal");
        lua_pushcfunction(state, meshUv); lua_setglobal(state, "mesh_uv");
        lua_pushcfunction(state, meshTriangle); lua_setglobal(state, "mesh_triangle");
        lua_pushcfunction(state, meshUpload); lua_setglobal(state, "mesh_upload");
        lua_pushcfunction(state, createMeshObject);
        lua_setglobal(state, "scene_mesh");
        lua_pushcfunction(state, removeBox);
        lua_setglobal(state, "scene_remove");
        lua_pushcfunction(state, moveBox);
        lua_setglobal(state, "scene_move");
        lua_pushcfunction(state, rotateBox);
        lua_setglobal(state, "scene_rotate");
        lua_pushcfunction(state, resizeBox);
        lua_setglobal(state, "scene_resize");
        lua_pushcfunction(state, colourBox);
        lua_setglobal(state, "scene_colour");
        lua_pushcfunction(state, hitObject);
        lua_setglobal(state, "scene_hit");
        return 0;
    }

    static int initializeInteraction(lua_State* state) {
        lua_pushcfunction(state, setInteractive);
        lua_setglobal(state, "scene_interactive");
        lua_pushcfunction(state, captureClicks);
        lua_setglobal(state, "input_capture_clicks");
        return 0;
    }

    void close() {
        if (state) lua_close(state);
        state = nullptr;
        memory = {};
        scene = {};
        models = {};
        meshes = {};
        meshStates = {};
        nextId = 1;
        assets = {};
        assetNames.clear();
        world = {};
        view = {};
        layout = {};
        storage = nullptr;
        storageError.clear();
        storageCalls = 0;
        storageWriteTokens = 8;
        storageWriteTime = std::chrono::steady_clock::now();
        sounds = {};
        samples = {};
        messages = {};
        messagingGranted = false;
        eventCallbackCount = 0;
        nextSampleVoice = 1;
        engineSound = {};
        hud = {};
        impulse = {};
        controlOffer = {};
        controlPose = {};
        controlGranted = controlActive = false;
        clickCapture = handlingClickPress = false;
        worldCalls = 0;
        frame = 0;
        delta = 0;
        framePrepared = false;
    }
    ~Impl() { close(); }

    bool dispatch(const LifecycleCall& call, std::string& error) {
        worldCalls = 0;
        storageCalls = 0;
        lua_sethook(state, instructionLimit, LUA_MASKCOUNT, kInstructionLimit);
        lua_pushcfunction(state, dispatchLifecycle);
        lua_pushlightuserdata(state, const_cast<LifecycleCall*>(&call));
        const int status = lua_pcall(state, 1, 0, 0);
        if (status != LUA_OK) {
            const char* message = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : nullptr;
            error = message ? message : "Script callback failed";
            close();
            return false;
        }
        error.clear();
        return true;
    }

    bool dispatchEvent(const MessageEvent& event, std::string& error) {
        worldCalls = 0;
        storageCalls = 0;
        lua_sethook(state, instructionLimit, LUA_MASKCOUNT, kInstructionLimit);
        lua_pushcfunction(state, dispatchRegisteredEvent);
        lua_pushlightuserdata(state, const_cast<MessageEvent*>(&event));
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            const char* message = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : nullptr;
            error = message ? message : "Event callback failed";
            close();
            return false;
        }
        error.clear();
        return true;
    }
};

PluginScriptRuntime::PluginScriptRuntime() : impl_(std::make_unique<Impl>()) {}
PluginScriptRuntime::~PluginScriptRuntime() = default;

bool PluginScriptRuntime::start(const std::string& source, std::string& error, bool sceneGranted,
                                const WorldQueries* world, const AssetCounts* assets, const ViewQuery* view,
                                bool soundGranted, bool hudGranted, const LayoutQuery* layout, bool impulseGranted,
                                bool controlGranted, bool inputGranted, GameplayStorage* storage, bool messagingGranted,
                                std::uint64_t initialFrame) {
    if (impl_->state) { error = "Script runtime is already running"; return false; }
    if (source.empty() || source.size() > kPluginTextLimit) { error = "Script is empty or exceeds 10 MiB"; return false; }
    if (world && (!world->floor || !world->resolve || !world->available)) { error = "Invalid world query service"; return false; }
    if (view && !view->pose) { error = "Invalid view service"; return false; }
    if (layout && !layout->cell) { error = "Invalid layout service"; return false; }
    if (assets && (assets->models < 0 || assets->models > 32 || assets->textures < 0 || assets->textures > 32 ||
                   assets->models + assets->textures + assets->audioNames.size() > 32 || !sceneGranted ||
                   (!assets->modelNames.empty() && assets->modelNames.size() != static_cast<size_t>(assets->models)) ||
                   (!assets->textureNames.empty() && assets->textureNames.size() != static_cast<size_t>(assets->textures)))) {
        error = "Invalid scene asset catalog";
        return false;
    }
    if (assets) {
        for (const auto* names : {&assets->modelNames, &assets->textureNames, &assets->audioNames}) {
            for (size_t i = 0; i < names->size(); ++i) {
                if ((*names)[i].empty() || (*names)[i].size() > 87) {
                    error = "Invalid scene asset name";
                    return false;
                }
                for (size_t j = 0; j < i; ++j) {
                    if ((*names)[i] == (*names)[j]) {
                        error = "Duplicate scene asset name";
                        return false;
                    }
                }
            }
        }
    }
    impl_->state = lua_newstate(limitedAlloc, &impl_->memory);
    if (!impl_->state) { error = "Cannot allocate script runtime"; return false; }
    if (world) impl_->world = *world;
    if (view) impl_->view = *view;
    if (layout) impl_->layout = *layout;
    impl_->storage = storage;
    impl_->messagingGranted = messagingGranted;
    impl_->startedAt = std::chrono::steady_clock::now();
    impl_->randomEngine.seed(static_cast<std::uint64_t>(impl_->startedAt.time_since_epoch().count()));
    impl_->frame = initialFrame;
    impl_->controlGranted = controlGranted;
    if (assets) {
        try {
            impl_->assets = *assets;
            impl_->assetNames = assets->modelNames;
            impl_->assetNames.insert(impl_->assetNames.end(), assets->textureNames.begin(), assets->textureNames.end());
            impl_->assetNames.insert(impl_->assetNames.end(), assets->audioNames.begin(), assets->audioNames.end());
            std::sort(impl_->assetNames.begin(), impl_->assetNames.end());
            if (std::adjacent_find(impl_->assetNames.begin(), impl_->assetNames.end()) != impl_->assetNames.end()) {
                error = "Duplicate gameplay asset name";
                impl_->close();
                return false;
            }
        } catch (const std::bad_alloc&) {
            error = "Cannot allocate scene asset catalog";
            impl_->close();
            return false;
        }
    }
    lua_State* state = impl_->state;
    *static_cast<Impl**>(lua_getextraspace(state)) = impl_.get();
    lua_sethook(state, instructionLimit, LUA_MASKCOUNT, kInstructionLimit);
    lua_pushcfunction(state, initializeGameplay);
    int status = lua_pcall(state, 0, 0, 0);
    if (status == LUA_OK) {
        lua_pushcfunction(state, Impl::initializeTime);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && world) {
        lua_pushcfunction(state, Impl::initializeWorld);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && view) {
        lua_pushcfunction(state, Impl::initializeView);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && layout) {
        lua_pushcfunction(state, Impl::initializeLayout);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && soundGranted) {
        lua_pushcfunction(state, Impl::initializeSound);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && hudGranted) {
        lua_pushcfunction(state, Impl::initializeHud);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && impulseGranted) {
        lua_pushcfunction(state, Impl::initializeImpulse);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && controlGranted) {
        lua_pushcfunction(state, Impl::initializeControl);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && sceneGranted) {
        lua_pushcfunction(state, Impl::initializeScene);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && sceneGranted && inputGranted) {
        lua_pushcfunction(state, Impl::initializeInteraction);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && storage) {
        lua_pushcfunction(state, Impl::initializeStorage);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK && messagingGranted) {
        lua_pushcfunction(state, Impl::initializeMessaging);
        status = lua_pcall(state, 0, 0, 0);
    }
    if (status == LUA_OK) status = loadSource(state, impl_->memory, source);
    if (status == LUA_OK) status = lua_pcall(state, 0, 1, 0);
    if (status != LUA_OK) {
        const char* message = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : nullptr;
        error = message ? message : "Script failed";
        impl_->close();
        return false;
    }
    if (!lua_istable(state, -1)) {
        error = "Script must return a lifecycle table";
        impl_->close();
        return false;
    }
    lua_pushcfunction(state, storeLifecycle);
    lua_insert(state, -2);
    status = lua_pcall(state, 1, 0, 0);
    if (status != LUA_OK) {
        const char* message = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : nullptr;
        error = message ? message : "Invalid lifecycle table";
        impl_->close();
        return false;
    }
    return impl_->dispatch({"on_start", nullptr, 0, 0}, error);
}

bool PluginScriptRuntime::tick(double seconds, std::string& error) {
    if (!impl_->state) { error = "Script runtime is not running"; return false; }
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1) {
        error = "Invalid tick duration";
        return false;
    }
    if (!impl_->framePrepared && impl_->frame < static_cast<std::uint64_t>(INT64_MAX)) ++impl_->frame;
    impl_->framePrepared = false;
    impl_->delta = seconds;
    return impl_->dispatch({"on_tick", nullptr, seconds, 1}, error);
}

void PluginScriptRuntime::prepareFrame(std::uint64_t frame) {
    impl_->frame = frame;
    impl_->framePrepared = true;
}

bool PluginScriptRuntime::input(const std::string& control, double value, std::string& error) {
    if (!impl_->state) { error = "Script runtime is not running"; return false; }
    if (control.empty() || control.size() > 32 || !std::isfinite(value) || value < -1 || value > 1) {
        error = "Invalid input event";
        return false;
    }
    for (char c : control) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            error = "Invalid input event";
            return false;
        }
    }
    impl_->handlingClickPress = control == "click" && value == 1;
    const bool ok = impl_->dispatch({"on_input", control.c_str(), value, 2}, error);
    impl_->handlingClickPress = false;
    return ok;
}

bool PluginScriptRuntime::message(std::string_view sender, const MessageEvent& event, std::string& error) {
    if (!impl_->state || !impl_->messagingGranted) { error = "Gameplay messaging is unavailable"; return false; }
    if (sender.empty() || sender.size() > 80 || event.topicSize == 0 ||
        event.topicSize > event.topic.size() || event.payloadSize > event.payload.size() ||
        (event.type != MessageEvent::Type::String && event.type != MessageEvent::Type::Number &&
         event.type != MessageEvent::Type::Boolean) ||
        (event.type == MessageEvent::Type::Number && !std::isfinite(event.number))) {
        error = "Invalid gameplay message";
        return false;
    }
    if (!impl_->dispatch({"on_message", sender.data(), 0, 3, &event, sender.size()}, error)) return false;
    return impl_->dispatchEvent(event, error);
}

bool PluginScriptRuntime::interactiveHit(const double* origin, const double* direction, double& distance) const {
    if (!impl_->state) return false;
    bool found = false;
    double closest = 14.0;
    auto consider = [&](const Box& box) {
        double hit = 0;
        if (box.id && box.interactive && gameplayRayHit(box, origin, direction, closest, hit)) {
            closest = hit;
            found = true;
        }
    };
    for (const auto& box : impl_->scene) consider(box);
    for (const auto& model : impl_->models) consider(model.transform);
    if (found) distance = closest;
    return found;
}

bool PluginScriptRuntime::capturesClicks() const { return impl_->state && impl_->clickCapture; }

bool PluginScriptRuntime::stop(std::string& error) {
    if (!impl_->state) { error.clear(); return true; }
    const bool ok = impl_->dispatch({"on_stop", nullptr, 0, 0}, error);
    impl_->close();
    return ok;
}

bool PluginScriptRuntime::running() const { return impl_->state != nullptr; }

std::vector<PluginScriptRuntime::Box> PluginScriptRuntime::boxes() const {
    std::vector<Box> result;
    for (const auto& box : impl_->scene) if (box.id) result.push_back(box);
    return result;
}

std::vector<PluginScriptRuntime::Model> PluginScriptRuntime::models() const {
    std::vector<Model> result;
    for (const auto& model : impl_->models) if (model.transform.id) result.push_back(model);
    return result;
}

const GameplayModel* PluginScriptRuntime::proceduralModel(int id) const {
    if (!impl_->state || id <= 0 || static_cast<size_t>(id) > impl_->meshes.size()) return nullptr;
    const auto& mesh = impl_->meshes[static_cast<size_t>(id - 1)];
    return impl_->meshStates[static_cast<size_t>(id - 1)] == 2 ? &mesh : nullptr;
}

PluginScriptRuntime::SoundBatch PluginScriptRuntime::takeSounds() {
    auto result = impl_->sounds;
    impl_->sounds = {};
    return result;
}

PluginScriptRuntime::SampleBatch PluginScriptRuntime::takeSamples() {
    auto result = impl_->samples;
    impl_->samples = {};
    return result;
}

PluginScriptRuntime::MessageBatch PluginScriptRuntime::takeMessages() {
    auto result = impl_->messages;
    impl_->messages = {};
    return result;
}

PluginScriptRuntime::EngineSound PluginScriptRuntime::takeEngineSound() {
    const auto result = impl_->engineSound;
    impl_->engineSound = {};
    return result;
}

PluginScriptRuntime::HudText PluginScriptRuntime::hudText() const { return impl_->hud; }

PluginScriptRuntime::Impulse PluginScriptRuntime::takeImpulse() {
    const auto result = impl_->impulse;
    impl_->impulse = {};
    return result;
}

PluginScriptRuntime::ControlOffer PluginScriptRuntime::controlOffer() const { return impl_->controlOffer; }

PluginScriptRuntime::ControlPose PluginScriptRuntime::takeControlPose() {
    const auto result = impl_->controlPose;
    impl_->controlPose = {};
    return result;
}

bool PluginScriptRuntime::setControlActive(bool active, std::string& error) {
    if (!impl_->state || !impl_->controlGranted) {
        error = "Script control capability is unavailable";
        return false;
    }
    if (impl_->controlActive == active) { error.clear(); return true; }
    impl_->controlActive = active;
    impl_->controlPose = {};
    return impl_->dispatch({"on_input", "control", active ? 1.0 : 0.0, 2}, error);
}

bool runPluginScript(const std::string& source, std::string& result, std::string& error) {
    result.clear();
    error.clear();
    if (source.empty() || source.size() > kPluginTextLimit) { error = "Script is empty or exceeds 10 MiB"; return false; }
    Memory memory;
    lua_State* state = lua_newstate(limitedAlloc, &memory);
    if (!state) { error = "Cannot allocate script runtime"; return false; }
    lua_sethook(state, instructionLimit, LUA_MASKCOUNT, kInstructionLimit);
    lua_pushcfunction(state, initialize);
    int status = lua_pcall(state, 0, 0, 0);
    if (status == LUA_OK) status = loadSource(state, memory, source);
    if (status == LUA_OK) status = lua_pcall(state, 0, 1, 0);
    if (status != LUA_OK) {
        const char* message = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : nullptr;
        error = message ? message : "Script failed";
    } else if (lua_type(state, -1) != LUA_TSTRING) {
        error = "Script must return a text message";
    } else {
        size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        if (!length || length > 160) error = "Script message must be 1-160 printable ASCII characters";
        else {
            for (size_t i = 0; i < length; ++i)
                if (static_cast<unsigned char>(text[i]) < 32 || static_cast<unsigned char>(text[i]) > 126) {
                    error = "Script message must be printable ASCII";
                    break;
                }
            if (error.empty()) result.assign(text, length);
        }
    }
    lua_close(state);
    return error.empty();
}
}
