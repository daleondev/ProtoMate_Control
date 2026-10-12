#pragma once

#include <cstdint>
#include <pneumo/messaging.hpp>

namespace icc
{
    enum class ServiceProvider : std::uint8_t
    {
        IO,
        UI
    };

    // Shared application contracts. Both bridges register this catalog.
    struct Services
    {
    };
}
