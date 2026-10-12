#pragma once

#include <concepts>

namespace application
{
    // A compile-time lifecycle contract; each application owns its behavior.
    template<typename T>
    concept Application = requires(T& app) {
        { app.start() } -> std::same_as<void>;
        { app.requestStop() } -> std::same_as<void>;
        { app.join() } -> std::same_as<void>;
    };
}
