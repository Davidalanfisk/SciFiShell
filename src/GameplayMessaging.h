#pragma once

#include "PluginScript.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace sf {
class GameplayMessaging {
public:
    struct Message {
        std::array<char, 80> sender{};
        size_t senderSize = 0;
        std::uint64_t owner = 0, generation = 0;
        PluginScriptRuntime::MessageEvent event;
    };
    struct Batch { std::array<Message, 64> messages{}; size_t count = 0; };

    bool push(std::string_view sender, std::uint64_t owner, std::uint64_t generation,
              const PluginScriptRuntime::MessageEvent& event) {
        if (sender.empty() || sender.size() > 80 || !owner || !generation || pending_.count == pending_.messages.size())
            return false;
        auto& message = pending_.messages[pending_.count++];
        std::memcpy(message.sender.data(), sender.data(), sender.size());
        message.senderSize = sender.size();
        message.owner = owner;
        message.generation = generation;
        message.event = event;
        return true;
    }

    Batch take() {
        auto batch = pending_;
        pending_ = {};
        return batch;
    }

private:
    Batch pending_{};
};
}
