#include "PluginScript.h"

#include <iostream>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    std::string result, error;
    check(sf::runPluginScript("return 'HELLO ' .. (2 + 3)", result, error) && result == "HELLO 5", "valid execution");
    check(sf::runPluginScript("if unix_time() > 0 then return 'OK' end", result, error) && result == "OK", "clock API");
    check(!sf::runPluginScript("return os.execute('x')", result, error), "no OS library");
    check(!sf::runPluginScript("return require('io')", result, error), "no module loader");
    check(!sf::runPluginScript("while true do end", result, error) && error.find("instruction limit") != std::string::npos, "instruction budget");
    check(!sf::runPluginScript("local a='xx' for i=1,20 do a=a..a end return 'ok'", result, error) &&
          error.find("memory") != std::string::npos, "memory budget");
    check(!sf::runPluginScript("return 42", result, error), "non-string result rejected");
    check(!sf::runPluginScript("return '\\n'", result, error), "control characters rejected");
    check(!sf::runPluginScript(std::string("\x1bLua", 4), result, error), "bytecode rejected");
    check(sf::runPluginScript("return 'AFTER FAILURE'", result, error), "fresh runtime after failure");
    return failures ? 1 : 0;
}
