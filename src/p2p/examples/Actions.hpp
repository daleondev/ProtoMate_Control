#pragma once

#include <pneumo/messaging.hpp>

#include <cstdint>

namespace example
{
    using namespace pnm::meta::string::literals;

    enum class ActionProvider
    {
        IO,
        UI
    };

    // Fields describe actions; no Actions instance is constructed.
    // Goal = target count, Feedback = current count, Result = final count.
    // IO executes this simulated action; UI submits goals and receives progress.
    struct Actions
    {
        [[= ActionProvider::IO]]
        pnm::msg::Action<std::uint32_t, std::uint32_t, std::uint32_t> demo_io_count;

        // The name defaults to "demo/io/count". To override it, add a name
        // annotation to the same field, e.g. [[= ActionProvider::IO, = "demo/count"_fs]].
    };
}
