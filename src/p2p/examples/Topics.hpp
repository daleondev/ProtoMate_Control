#pragma once

#include <pneumo/messaging.hpp>

#include <string>

namespace example
{
    enum class Direction
    {
        IOToUI,
        UIToIO
    };

    // Both applications share this catalog; each has its own bus and bridge.
    struct Topics
    {
        [[= Direction::IOToUI]] static constexpr pnm::msg::TopicDefinition<std::string> state{
            "demo/io/state"
        };
        [[= Direction::UIToIO]] static constexpr pnm::msg::TopicDefinition<std::string> status{
            "demo/ui/status"
        };
    };
}
