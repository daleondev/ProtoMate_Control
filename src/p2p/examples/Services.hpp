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

    // IO provides this service; UI calls it. Replies return to the caller.
    struct Services
    {
        [[= ServiceProvider::IO]] static constexpr pnm::msg::ServiceDefinition<std::string, std::string> echo{
            "demo/io/echo"
        };
    };
}
