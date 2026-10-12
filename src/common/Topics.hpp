#pragma once

#include <pneumo/messaging.hpp>
#include <cstdint>

namespace application
{
    enum class TopicPublisher : std::uint8_t { IO, UI };

    struct LedState
    {
        std::uint32_t sequence{};
        bool on{};
    };

    struct Topics
    {
        // Serialized as "io/led/state". UI decides, IO applies the received state.
        [[= TopicPublisher::UI]] pnm::msg::Topic<LedState> io_led_state;
    };
}
