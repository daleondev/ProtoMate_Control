#pragma once

#include <pneumo/messaging.hpp>
#include <pneumo/meta.hpp>

#include <string>

namespace example
{
    using namespace pnm::meta::string::literals;

    enum class TopicPublisher
    {
        IO,
        UI
    };

    // Both applications share this catalog; each has its own bus and bridge.
    struct Topics
    {
        [[ = TopicPublisher::IO, = "demo/io/state"_fs ]] std::string state;
        [[ = TopicPublisher::UI, = "demo/ui/status"_fs ]] std::string status;
    };
}
