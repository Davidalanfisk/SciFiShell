#include "PluginScript.h"

#include <cstdlib>
#include <ctime>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace sf {
namespace {
constexpr size_t kMemoryLimit = 1024 * 1024;
constexpr int kInstructionLimit = 100000;
struct Memory { size_t used = 0; };

void* limitedAlloc(void* ud, void* ptr, size_t oldSize, size_t newSize) {
    auto& memory = *static_cast<Memory*>(ud);
    if (!ptr) oldSize = 0;
    if (!newSize) {
        std::free(ptr);
        memory.used -= oldSize;
        return nullptr;
    }
    if (newSize > oldSize && newSize - oldSize > kMemoryLimit - memory.used) return nullptr;
    void* next = std::realloc(ptr, newSize);
    if (next) memory.used = memory.used - oldSize + newSize;
    return next;
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
}

bool runPluginScript(const std::string& source, std::string& result, std::string& error) {
    result.clear();
    error.clear();
    if (source.empty() || source.size() > 16384) { error = "Script is empty or too large"; return false; }
    Memory memory;
    lua_State* state = lua_newstate(limitedAlloc, &memory);
    if (!state) { error = "Cannot allocate script runtime"; return false; }
    lua_sethook(state, instructionLimit, LUA_MASKCOUNT, kInstructionLimit);
    lua_pushcfunction(state, initialize);
    int status = lua_pcall(state, 0, 0, 0);
    if (status == LUA_OK) status = luaL_loadbufferx(state, source.data(), source.size(), "plugin", "t");
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
