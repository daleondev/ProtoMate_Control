#pragma once

#include <pneumo/messaging.hpp>

#include <string>

namespace example
{
    enum class ServiceProvider
    {
        IO,
        UI
    };

    // API example only: service forwarding is not implemented yet.
    // IO provides this service; UI calls it. Replies return to the caller.
    struct Services
    {
        [[= ServiceProvider::IO]] static constexpr pnm::msg::ServiceDefinition<std::string, std::string> echo{
            "demo/io/echo"
        };
    };
}
