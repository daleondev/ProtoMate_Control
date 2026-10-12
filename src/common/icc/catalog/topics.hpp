#pragma once

#include <cstdint>
#include <string>
#include "common/icc/transport/ITransport.hpp"
#include "common/motion/Motor.hpp"
#include <pneumo/messaging.hpp>

namespace icc
{
    enum class TopicPublisher : std::uint8_t
    {
        IO,
        UI
    };

    // Formatted text including its original timestamp/level. Leave room for
    // framing and the topic name within the 512-byte background frame limit.
    inline constexpr std::size_t MAX_LOG_BYTES{ 384 };
    inline constexpr pnm::msg::SubscriptionOptions LOG_SUBSCRIPTION{ .capacity = 16 };

    // Shared application contracts. Both bridges register this catalog.
    struct Topics
    {
        [[= TopicPublisher::IO, = Lane::Background, = LOG_SUBSCRIPTION]]
        pnm::msg::Topic<std::string> io_logs;

        [[= TopicPublisher::IO, = pnm::msg::SubscriptionOptions::latestOnly()]]
        pnm::msg::Topic<motion::MotorSnapshot> io_motor_state;
    };
}
