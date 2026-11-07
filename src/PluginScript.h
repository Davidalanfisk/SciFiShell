#pragma once

#include <string>

namespace sf {

// Each invocation gets a fresh Lua state with no standard libraries or ambient capabilities.
bool runPluginScript(const std::string& source, std::string& result, std::string& error);

}
