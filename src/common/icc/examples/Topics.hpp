#pragma once

#include <cstdint>
#include "common/icc/transport/ITransport.hpp"

#include <pneumo/messaging.hpp>

#include <string>

namespace example
{
    using namespace pnm::meta::string::literals;

    enum class TopicPublisher : std::uint8_t
    {
        IO,
        UI
    };

    // Fields declare the payload types; no Topics instance is constructed.
    // Both applications share this catalog; each has its own bus and bridge.
    struct Topics
    {
        [[= TopicPublisher::IO]] pnm::msg::Topic<std::string> demo_io_state;
        [[= TopicPublisher::UI, = "demo/ui/status"_fs]] pnm::msg::Topic<std::string> status;
        // FIFO events: independent background capacity, keep at most 16 records.
        [[= TopicPublisher::IO, = icc::Lane::Background,
          = pnm::msg::SubscriptionOptions{ .capacity = 16 }]]
        pnm::msg::Topic<std::string> demo_io_logs;
    };
}
