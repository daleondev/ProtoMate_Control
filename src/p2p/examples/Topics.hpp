#pragma once

#include <pneumo/messaging.hpp>

#include <string>

namespace example
{
    enum class TopicPublisher
    {
        IO,
        UI
    };

    // Both applications share this catalog; each has its own bus and bridge.
    struct Topics
    {
        [[= TopicPublisher::IO]] static constexpr pnm::msg::TopicDefinition<std::string> state{
            "demo/io/state"
        };
        [[= TopicPublisher::UI]] static constexpr pnm::msg::TopicDefinition<std::string> status{
            "demo/ui/status"
        };
    };
}
