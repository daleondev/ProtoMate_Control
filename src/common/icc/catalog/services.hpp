#pragma once

#include <pneumo/messaging.hpp>
#include <cstdint>

namespace application
{
    enum class ServiceProvider : std::uint8_t { IO, UI };

    // Shared application contracts. Both bridges register this catalog.
    struct Services
    {
    };
}
